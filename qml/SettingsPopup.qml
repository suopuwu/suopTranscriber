import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SuopTranscriber
import "CombineRuleRegex.js" as CombineRuleRegex

Popup {
    id: settingsPopup

    signal changeVaultRequested()

    // Combine-rule "test this rule" preview: detectCombineGroups() lives on
    // the controller, the vault's current file listing on the tree model —
    // neither of which this popup otherwise touches.
    property var transcriptionController: null
    property var vaultTreeModel: null

    // The settings dialog is split into pages selected from a
    // left-hand navigation rail. currentPage indexes into pageNames and gates
    // which per-section wrappers in contentColumn are visible, so the user
    // sees one page at a time instead of one giant scroll. Purely a
    // presentation reorg — every control/binding/save-hook is unchanged, only
    // grouped behind a page gate.
    readonly property var pageNames: [qsTr("Appearance"), qsTr("Sidebar & Layout"),
                                      qsTr("Transcription & AI"), qsTr("Recorder"), qsTr("Combine"),
                                      qsTr("Models")]
    property int currentPage: 0

    // Whether the (long) per-icon override grid is expanded. Collapsed by
    // default — the default-pack picker is the common choice; overriding
    // individual icons is the power-user tail.
    property bool iconOverridesExpanded: false

    // Rule-list-level overlap warnings (see computeRuleOverlaps()'s doc
    // comment). Deliberately NOT a plain `property var x: <binding>` —
    // vaultTreeModel.allAudioPaths() is a Q_INVOKABLE with no NOTIFY
    // signal, so QML's binding engine can't track "the vault's file list
    // changed" as a dependency at all, and on a freshly opened vault
    // AppSettings.loadFromVault() (which sets combineRules, the only real
    // tracked dependency here) runs BEFORE VaultTreeModel::rebuild()
    // populates that file list (see Main.qml's own doc comment on that
    // ordering) — so a declarative binding here would evaluate once, too
    // early, against an empty file list, and then never again (same
    // "frozen at initial value" class of bug as the dummy-dependency
    // trick elsewhere in this codebase, just via a genuinely-untracked
    // invokable instead). Recomputed imperatively instead: once whenever
    // this popup actually opens (by then the vault a real user is looking
    // at is definitely already loaded), and again on every combineRules
    // change while open (rule add/edit/differentiator toggle).
    property var combineOverlapWarnings: []
    function updateCombineOverlapWarnings() {
        settingsPopup.combineOverlapWarnings = settingsPopup.vaultTreeModel
                ? settingsPopup.computeRuleOverlaps(AppSettings.combineRules, settingsPopup.vaultTreeModel.allAudioPaths())
                : []
    }

    // Live previews of the exact system-prompt clause each slider currently
    // selects, shown as a subtitle under the slider so the 3-tier snap
    // (settingsPopup_autoLabelSlidersSnapToDiscreteTiers, tst_styleguide.cpp)
    // doesn't read as an arbitrary position — the user sees the actual
    // wording it produces. Mirrors labelPreludePhrasing() and the
    // tagReuseBias tiers in buildSystemPrompt() (LabelingEngine.cpp) at the
    // same 0.34/0.67 thresholds; keep both in sync if those wordings change.
    function wordinessPhrasing(value) {
        if (value >= 0.67)
            return qsTr("a descriptive label (up to a short phrase), a detailed multi-sentence prelude")
        if (value >= 0.34)
            return qsTr("a short display label (a few words), a one-paragraph prelude")
        return qsTr("a very short label (1-3 words), a single concise prelude sentence")
    }
    function tagReuseBiasPhrasing(value) {
        if (value >= 0.67)
            return qsTr("strongly prefer reusing an existing tag, even if only an approximate fit")
        if (value >= 0.34)
            return qsTr("reuse an existing tag if it reasonably fits, otherwise invent a new one")
        return qsTr("prefer inventing a new, precise tag unless an existing one is a very close match")
    }

    // Icon-pack settings (DESIGN.md "Iconography"). iconOverrides is a
    // name->packId map; QML only fires onIconOverridesChanged when the
    // property is REASSIGNED (mutating the existing object in place is
    // invisible to it), so both writers copy, edit, then assign the whole
    // object — same discipline combineRules uses.
    function setIconOverride(name, packId) {
        const o = Object.assign({}, AppSettings.iconOverrides)
        if (packId === "")   // "" = follow the default pack, i.e. no override
            delete o[name]
        else
            o[name] = packId
        AppSettings.iconOverrides = o
    }
    // Click-cycles one icon through: auto (follow default pack) -> each pack
    // in turn -> auto. A compact alternative to a per-row combo box for the
    // ~40-icon grid. An override naming a now-removed pack lands at index -1,
    // so the +1 wraps it back to "auto" — a self-healing reset.
    function cycleIconOverride(name) {
        const seq = [""].concat(IconTheme.packIds())
        const cur = AppSettings.iconOverrides[name] || ""
        settingsPopup.setIconOverride(name, seq[(seq.indexOf(cur) + 1) % seq.length])
    }

    // Bug 50: detectCombineGroups() itself is fully live (QFileInfo::exists() checked fresh
    // every call, no C++-side caching) — but each ruleCard's own previewGroups is a cached JS
    // property only recomputed on Component.onCompleted (once, ever, since this popup is a
    // persistent singleton — see Main.qml) or on a pattern-text edit. Without this, deleting a
    // combined output file (or any other disk change) while Settings is closed left a stale
    // "Already combined" preview showing until app restart, since simply reopening the popup
    // never re-ran updatePreview(). Same fix shape as updateCombineOverlapWarnings() above.
    onOpened: {
        settingsPopup.updateCombineOverlapWarnings()
        for (let i = 0; i < combineRuleRepeater.count; i++) {
            const item = combineRuleRepeater.itemAt(i)
            if (item)
                item.updatePreview()
        }
    }
    Connections {
        target: AppSettings
        function onCombineRulesChanged() {
            if (settingsPopup.opened)
                settingsPopup.updateCombineOverlapWarnings()
        }
    }

    width: 640
    // Sized to the ACTIVE page: contentColumn.implicitHeight reflects only the
    // currently-visible section wrappers (a ColumnLayout excludes invisible
    // children), so the popup grows/shrinks per page and caps at maxHeight
    // (then the ScrollView scrolls). Also floored to fit the nav rail.
    height: Math.min(maxHeight,
                     Math.max(navColumn.implicitHeight + 32,
                              headerRow.implicitHeight + contentColumn.implicitHeight + 42))
    // Modal: Overlay.overlay's automatic block-plane now genuinely blocks
    // clicks/drags to Sidebar/MainPanel/BottomBar/TitleBar while this is
    // open (a previous `modal: false` + input-swallowing MouseArea
    // workaround in Main.qml correctly blocked hover but let TapHandler
    // clicks pass through — TapHandlers use passive-grab semantics
    // independent of that MouseArea's exclusive Item-grab). The window-edge
    // ResizeGrips (Main.qml) stay usable despite the block-plane covering
    // the whole window: they're reparented onto Overlay.overlay itself with
    // a z above the popup's own block-plane/content, so they keep
    // painting/receiving input on top of it. `dim` is still set explicitly
    // below (not left to just follow `modal`) for the slider-drag
    // passthrough behavior.
    modal: true
    focus: true
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0
    // Suspended (NoAutoClose) while middleDragScrollArea is armed: Escape
    // and click-outside are how Chrome-style autoscroll cancels itself too,
    // and without this the popup's own CloseOnEscape/CloseOnPressOutside
    // would race the autoscroll's own cancel handling and close the whole
    // Settings popup instead of just disarming the scroll.
    closePolicy: middleDragScrollArea.armed
                 ? Popup.NoAutoClose
                 : (Popup.CloseOnEscape | Popup.CloseOnPressOutside)

    readonly property real maxHeight: parent ? parent.height * 0.9 : 600

    // While any slider in here is actively being dragged, hide this popup's
    // modal dimmer and its own visual content so the user can see the live
    // effect on the app behind it — reappears the instant the drag ends.
    // The Popup itself stays open/modal (visible/opacity unchanged) so the
    // slider's mouse grab isn't interrupted mid-drag.
    property bool anyPressed: radiusSlider.pressed || sidebarOpacitySlider.pressed
                               || mainOpacitySlider.pressed || rimWidthSlider.pressed
                               || highlightRadiusSlider.pressed
                               || timestampDecimalPlacesSlider.pressed
                               || progressGlowWidthSlider.pressed || progressGlowRadiusSlider.pressed
                               || progressGlowLayerCountSlider.pressed
                               || sidebarEdgeLeftMarginSlider.pressed || sidebarContentLeftMarginSlider.pressed
                               || sidebarRightMarginSlider.pressed

    dim: !anyPressed

    // Covers every key in AppSettings.defaults; used to enable/disable the
    // global "Reset to defaults" button. Reactive because it only reads
    // AppSettings properties (via isDefault), no manual signal wiring needed.
    function anySettingDirty() {
        return !AppSettings.isDefault("borderRadius")
                || !AppSettings.isDefault("useSystemTitlebar")
                || !AppSettings.isDefault("accentColor")
                || !AppSettings.isDefault("sidebarBackgroundColor")
                || !AppSettings.isDefault("sidebarOpacity")
                || !AppSettings.isDefault("sidebarBlur")
                || !AppSettings.isDefault("mainBackgroundColor")
                || !AppSettings.isDefault("mainOpacity")
                || !AppSettings.isDefault("mainBlur")
                || !AppSettings.isDefault("rimEnabled")
                || !AppSettings.isDefault("rimWidth")
                || !AppSettings.isDefault("rimColor")
                || !AppSettings.isDefault("recordingActiveColor")
                || !AppSettings.isDefault("recordingFormat")
                || !AppSettings.isDefault("highlightColor")
                || !AppSettings.isDefault("highlightRadius")
                || !AppSettings.isDefault("sidebarSelectionColor")
                || !AppSettings.isDefault("sidebarSelectionBorderColor")
                || !AppSettings.isDefault("useLabelsMainPanel")
                || !AppSettings.isDefault("useLabelsSidebar")
                || !AppSettings.isDefault("hideExtensionsSidebar")
                || !AppSettings.isDefault("hideExtensionsMainPanel")
                || !AppSettings.isDefault("autoLabelEnabled")
                || !AppSettings.isDefault("restrictTagsToExisting")
                || !AppSettings.isDefault("tagReuseBias")
                || !AppSettings.isDefault("wordiness")
                || !AppSettings.isDefault("chapterDensity")
                || !AppSettings.isDefault("timestampDecimalPlaces")
                || !AppSettings.isDefault("progressGlowEnabled")
                || !AppSettings.isDefault("progressGlowColor")
                || !AppSettings.isDefault("progressGlowWidth")
                || !AppSettings.isDefault("progressGlowRadius")
                || !AppSettings.isDefault("progressGlowLayerCount")
                || !AppSettings.isDefault("progressGlowHidesBar")
                || !AppSettings.isDefault("bottomBarTaskClickOpensFile")
                || !AppSettings.isDefault("bottomBarTaskTooltip")
                || !AppSettings.isDefault("bottomBarPathShrinkToContent")
                || !AppSettings.isDefault("bottomBarTaskTextFit")
                || !AppSettings.isDefault("deleteOriginalsAfterCombine")
                || !AppSettings.isDefault("autoCombineAfterGrab")
                || !AppSettings.isDefault("sidebarEdgeLeftMargin")
                || !AppSettings.isDefault("sidebarContentLeftMargin")
                || !AppSettings.isDefault("sidebarRightMargin")
    }

    // Bug 53: every write below reassigns the WHOLE AppSettings.combineRules
    // array, which (being a plain JS array, no incremental model
    // notifications possible) makes the rule-list Repeater destroy and
    // recreate every rule card on every single edit — see
    // feedback_repeater_array_rebuild_eats_click memory. That rebuild was
    // observed to scroll the Settings section back to the top; restoring
    // scrollArea's contentY across the reassignment fixes the symptom
    // directly regardless of the exact internal trigger (Qt.callLater so the
    // restore lands after the rebuild's own layout pass settles, not before
    // it can re-clamp).
    function commitCombineRules(newRules) {
        const savedContentY = scrollArea.contentItem ? scrollArea.contentItem.contentY : -1
        AppSettings.combineRules = newRules
        if (savedContentY >= 0) {
            Qt.callLater(function () {
                if (scrollArea.contentItem)
                    scrollArea.contentItem.contentY = savedContentY
            })
        }
    }

    // Rule mutation helpers: AppSettings.combineRules is a plain JS array
    // (see its own doc comment in AppSettings.qml — no per-value reset
    // semantics fit it), so every edit rebuilds and reassigns the whole
    // array to trigger combineRulesChanged. All four route through
    // commitCombineRules() above rather than assigning AppSettings.combineRules
    // directly, so scroll position is preserved consistently everywhere.
    function updateRule(index, key, value) {
        const rules = AppSettings.combineRules.slice()
        rules[index] = Object.assign({}, rules[index], {[key]: value})
        settingsPopup.commitCombineRules(rules)
    }
    // The ONLY writer of rule `pattern` — every pattern edit (button-driven
    // differentiator toggle, or hand-typed text in rulePatternField) must
    // route through here rather than a plain updateRule(index, "pattern",
    // ...), because it also re-derives/heals sortGroupIndex against the NEW
    // pattern (see resolveSortGroupIndex()'s doc comment for why a stored
    // index alone can't survive pattern edits, and why sortGroupOccurrence
    // rides alongside sortGroupText to disambiguate duplicate-text runs).
    // `overrideSortGroupText`/`overrideSortGroupOccurrence`, if given,
    // REPLACE the rule's current sort-differentiator designation (used when
    // the user just picked a different run to sort by, or cleared it with
    // ""); omitted, they keep whatever the rule already had, just
    // re-resolved against `newPattern`.
    function commitPatternChange(index, newPattern, overrideSortGroupText, overrideSortGroupOccurrence) {
        const rule = AppSettings.combineRules[index]
        const targetText = overrideSortGroupText !== undefined ? overrideSortGroupText : (rule.sortGroupText || "")
        const targetOccurrence = overrideSortGroupText !== undefined
                ? (overrideSortGroupOccurrence || 0) : (rule.sortGroupOccurrence || 0)
        const sortGroupIndex = CombineRuleRegex.resolveSortGroupIndex(newPattern, targetText, targetOccurrence)
        const rules = AppSettings.combineRules.slice()
        rules[index] = Object.assign({}, rules[index], {
            pattern: newPattern,
            sortGroupText: sortGroupIndex > 0 ? targetText : "",
            sortGroupOccurrence: sortGroupIndex > 0 ? targetOccurrence : 0,
            sortGroupIndex: sortGroupIndex
        })
        settingsPopup.commitCombineRules(rules)
    }
    function addRule() {
        settingsPopup.commitCombineRules(AppSettings.combineRules.concat(
                    [{name: qsTr("New rule"), pattern: "", enabled: true}]))
    }
    function removeRule(index) {
        const rules = AppSettings.combineRules.slice()
        rules.splice(index, 1)
        settingsPopup.commitCombineRules(rules)
    }

    // The Combine-page regex/filename highlighter's pure JS helpers
    // (escapeRegexPreviewHtml, highlightPattern, hasUngroupedAlternation,
    // the tokenizer/differentiator-run machinery, classifyFilenameHtml, …)
    // live in CombineRuleRegex.js — a stateless `.pragma library` module,
    // since none of them touch this popup's ids, properties, or AppSettings.
    // Call them as CombineRuleRegex.<name>(...). computeRuleOverlaps() below
    // stays here because it reads settingsPopup.transcriptionController.

    // Non-blocking "these rules might conflict" warnings for the combine-
    // rule list as a whole — general regex disjointness is undecidable for
    // QRegularExpression's backtracking dialect (see
    // hasUngroupedAlternation()'s doc comment for a taste of how gnarly
    // that gets), so this only catches what's actually checkable: (a) two
    // enabled rules with byte-identical pattern text (the "obvious" static
    // case — zero false positives, catches the copy-paste-forgot-to-edit
    // mistake), and (b) two enabled rules that empirically both match the
    // SAME file currently in the vault, reusing detectCombineGroups()'s
    // real matching (no new C++ needed — a file matched by 2+ rules today
    // silently gets queued into 2+ separate combine jobs, since
    // findCombineGroups() has no first-rule-wins dedup). Returns an object
    // keyed by ruleName -> array of {other: otherRuleName, file: "" for the
    // static case, or the shared filename for the empirical case}.
    function computeRuleOverlaps(rules, audioPaths) {
        const warnings = {}
        function addWarning(ruleName, other, file) {
            if (!warnings[ruleName])
                warnings[ruleName] = []
            warnings[ruleName].push({other: other, file: file})
        }

        const enabled = rules.filter(function (r) { return r.enabled && r.pattern.length > 0 })
        for (let i = 0; i < enabled.length; i++) {
            for (let j = i + 1; j < enabled.length; j++) {
                if (enabled[i].pattern === enabled[j].pattern) {
                    addWarning(enabled[i].name, enabled[j].name, "")
                    addWarning(enabled[j].name, enabled[i].name, "")
                }
            }
        }

        if (settingsPopup.transcriptionController && enabled.length > 0) {
            const groups = settingsPopup.transcriptionController.detectCombineGroups(enabled, audioPaths)
            const fileToRules = {}
            for (const g of groups) {
                for (const f of g.files) {
                    // detectCombineGroups() returns full paths — shorten to
                    // just the filename for display, matching every other
                    // file-listing spot in this popup (e.g. the preview
                    // cards' own f.substring(f.lastIndexOf("/") + 1) below,
                    // and differentiatorNonParticipatingFiles(), which
                    // already returns bare filenames from the C++ side).
                    const name = f.substring(f.lastIndexOf("/") + 1)
                    if (!fileToRules[name])
                        fileToRules[name] = []
                    if (fileToRules[name].indexOf(g.ruleName) < 0)
                        fileToRules[name].push(g.ruleName)
                }
            }
            for (const f in fileToRules) {
                const names = fileToRules[f]
                if (names.length > 1) {
                    for (let i = 0; i < names.length; i++)
                        for (let j = 0; j < names.length; j++)
                            if (i !== j)
                                addWarning(names[i], names[j], f)
                }
            }
        }
        return warnings
    }

    background: Rectangle {
        color: Theme.surfaceDialog
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: Theme.border
        opacity: settingsPopup.anyPressed ? 0 : 1
    }

    ColorDialog {
        id: sidebarColorDialog
        title: qsTr("Sidebar Background Color")
        selectedColor: AppSettings.sidebarBackgroundColor
        onAccepted: AppSettings.sidebarBackgroundColor = selectedColor
    }

    ColorDialog {
        id: mainColorDialog
        title: qsTr("Main Panel Background Color")
        selectedColor: AppSettings.mainBackgroundColor
        onAccepted: AppSettings.mainBackgroundColor = selectedColor
    }

    ColorDialog {
        id: rimColorDialog
        title: qsTr("Rim Color")
        selectedColor: AppSettings.rimColor
        options: ColorDialog.ShowAlphaChannel
        onAccepted: AppSettings.rimColor = selectedColor
    }

    ColorDialog {
        id: recordingActiveColorDialog
        title: qsTr("Recording Indicator Color")
        selectedColor: AppSettings.recordingActiveColor
        options: ColorDialog.ShowAlphaChannel
        onAccepted: AppSettings.recordingActiveColor = selectedColor
    }

    ColorDialog {
        id: accentColorDialog
        title: qsTr("Accent Color")
        selectedColor: AppSettings.accentColor
        onAccepted: AppSettings.accentColor = selectedColor
    }

    ColorDialog {
        id: highlightColorDialog
        title: qsTr("Sentence Highlight Color")
        selectedColor: AppSettings.highlightColor
        onAccepted: AppSettings.highlightColor = selectedColor
    }

    ColorDialog {
        id: sidebarSelectionColorDialog
        title: qsTr("Sidebar Selection Color")
        selectedColor: AppSettings.sidebarSelectionColor
        options: ColorDialog.ShowAlphaChannel
        onAccepted: AppSettings.sidebarSelectionColor = selectedColor
    }

    ColorDialog {
        id: sidebarSelectionBorderColorDialog
        title: qsTr("Sidebar Selection Border Color")
        selectedColor: AppSettings.sidebarSelectionBorderColor
        options: ColorDialog.ShowAlphaChannel
        onAccepted: AppSettings.sidebarSelectionBorderColor = selectedColor
    }

    ColorDialog {
        id: progressGlowColorDialog
        title: qsTr("Progress Glow Color")
        selectedColor: AppSettings.progressGlowColor
        options: ColorDialog.ShowAlphaChannel
        onAccepted: AppSettings.progressGlowColor = selectedColor
    }

    // Left navigation rail — one entry per page in pageNames. Each
    // entry sets currentPage; the matching section wrappers in contentColumn
    // show, the rest hide. Anchored (not in a Layout) so scrollArea/the
    // middle-drag overlay can anchor to its right edge as siblings.
    ColumnLayout {
        id: navColumn
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: 16
        anchors.topMargin: 16
        anchors.bottomMargin: 16
        width: 150
        spacing: 4

        Repeater {
            model: settingsPopup.pageNames
            delegate: Button {
                id: navButton
                required property int index
                required property string modelData
                readonly property bool selected: settingsPopup.currentPage === navButton.index
                Layout.fillWidth: true
                text: modelData
                hoverEnabled: true
                onClicked: settingsPopup.currentPage = navButton.index

                contentItem: Text {
                    text: navButton.text
                    color: "white"
                    font.pixelSize: 13
                    font.bold: navButton.selected
                    leftPadding: 8
                    horizontalAlignment: Text.AlignLeft
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
                background: Rectangle {
                    radius: AppSettings.borderRadius
                    color: navButton.selected
                            ? Qt.rgba(AppSettings.highlightColor.r, AppSettings.highlightColor.g,
                                      AppSettings.highlightColor.b, 0.22)
                            : (navButton.hovered ? "#2f2f2f" : "transparent")
                    border.width: navButton.selected ? 1 : 0
                    border.color: AppSettings.highlightColor
                }
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }
                HoverBorder {
                    anchors.fill: parent
                    active: navButton.hovered
                }
            }
        }
        Item { Layout.fillHeight: true }
    }

    // Persistent dialog chrome — title + global actions, reachable from every
    // page (moved out of the scrolling content).
    RowLayout {
        id: headerRow
        anchors.left: navColumn.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 12
        anchors.topMargin: 16
        anchors.rightMargin: 16
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: qsTr("Settings")
            color: "white"
            font.bold: true
            font.pixelSize: 16
        }
        Button {
            id: changeVaultButton
            text: qsTr("Change Vault")
            hoverEnabled: true
            onClicked: settingsPopup.changeVaultRequested()

            background: Rectangle {
                radius: AppSettings.borderRadius
                color: (changeVaultButton.hovered && changeVaultButton.enabled) ? Theme.surfaceHover : Theme.surface
                border.width: 1
                border.color: Theme.border
            }
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
            HoverBorder {
                anchors.fill: parent
                active: changeVaultButton.hovered
            }
        }
        Button {
            id: resetAllButton
            text: qsTr("Reset to defaults")
            enabled: settingsPopup.anySettingDirty()
            opacity: enabled ? 1 : 0.35
            hoverEnabled: enabled
            onClicked: AppSettings.resetAll()

            background: Rectangle {
                radius: AppSettings.borderRadius
                color: (resetAllButton.hovered && resetAllButton.enabled) ? Theme.surfaceHover : Theme.surface
                border.width: 1
                border.color: Theme.border
            }
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
                enabled: resetAllButton.enabled
            }
            HoverBorder {
                anchors.fill: parent
                active: resetAllButton.enabled && resetAllButton.hovered
            }
        }
    }

    ScrollView {
        id: scrollArea
        anchors.left: navColumn.right
        anchors.right: parent.right
        anchors.top: headerRow.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: 12
        anchors.topMargin: 10
        anchors.bottomMargin: 16
        // Reduced by the same 14px contentColumn reserves below for the
        // scrollbar overlay — keeps the CONTENT symmetric by giving the
        // scrollbar its own lane inside the right margin.
        anchors.rightMargin: 2
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        opacity: settingsPopup.anyPressed ? 0 : 1

        // Bug 54: horizontal scrolling is never wanted in this popup (the
        // scrollbar above is already permanently AlwaysOff) — but Right-arrow
        // at the end of a TextField's text (e.g. rulePatternField) could
        // still drag the underlying Flickable's contentX away from 0 (Qt
        // Quick's cursor-follow/ensure-visible behavior for a TextInput
        // nested in a Flickable), letting the whole popup pan offscreen.
        // A plain `Binding{value:0}` does NOT hold here — verified with an
        // offscreen QTest repro: since the value has no reactive dependency,
        // QML applies it once and any later direct write to contentX (which
        // is exactly what the cursor-follow mechanism does) simply overrides
        // it with no further reassertion. An explicit onContentXChanged reset
        // (confirmed via the same repro to snap a manual drift straight back
        // to 0) is what's actually needed.
        Connections {
            target: scrollArea.contentItem
            function onContentXChanged() {
                if (scrollArea.contentItem.contentX !== 0)
                    scrollArea.contentItem.contentX = 0
            }
        }

        ColumnLayout {
            id: contentColumn
            // NOT parent.width: a QML child declared directly inside
            // ScrollView sits in its internal Flickable's contentItem, whose
            // own width is otherwise driven BY this same child's implicit
            // size when nothing else constrains it — "width: parent.width"
            // is a binding loop that settles at contentColumn's own
            // (unclamped) implicitWidth instead of the real viewport width.
            // Binding to the ScrollView's own id breaks the loop: verified
            // empirically (offscreen qml repro) that parent.width let a
            // slider row's content stretch past the popup's actual bounds
            // even though every individual row fit under naive arithmetic.
            //
            // -14: the Basic style's vertical ScrollBar is an OVERLAY —
            // `parent: control; x: control.width - width` (confirmed in
            // /usr/lib/qt6/qml/QtQuick/Controls/Basic/ScrollView.qml) — it
            // reserves no layout space and floats flush against the
            // ScrollView's right edge, right on top of content that fills
            // the full width (e.g. the per-row reset buttons). Leaving a
            // clearance gap here keeps it from covering them.
            width: scrollArea.width - 14
            clip: true
            spacing: 10

            // ===== Page 0: Appearance section =====
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 0

            Label {
                text: qsTr("Appearance")
                color: "white"
                font.bold: true
            }

            // --- Icon pack -------------------------------------------------
            // Default pack picker: a segmented chip row (there are only a
            // couple of packs, so this reads clearer than a combo box). Each
            // chip previews the pack by rendering a few of its own icons.
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    Layout.alignment: Qt.AlignVCenter
                    wrapMode: Text.WordWrap
                    color: "white"
                    text: qsTr("Icon pack")
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: IconTheme.packIds()
                        Item {
                            id: packChip
                            required property string modelData
                            readonly property bool current: AppSettings.iconPack === packChip.modelData
                            implicitWidth: packChipRow.implicitWidth + 20
                            implicitHeight: 30

                            ToolTip.visible: packChipHover.hovered
                            ToolTip.text: IconTheme.packDescription(packChip.modelData)
                            ToolTip.delay: 500

                            HoverHandler { id: packChipHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: AppSettings.iconPack = packChip.modelData }

                            Rectangle {
                                anchors.fill: parent
                                radius: AppSettings.borderRadius
                                color: packChip.current ? "#33ffffff"
                                       : (packChipHover.hovered ? Theme.surfaceHover : Theme.surface)
                                border.width: 1
                                border.color: packChip.current ? AppSettings.accentColor : Theme.border
                            }
                            RowLayout {
                                id: packChipRow
                                anchors.centerIn: parent
                                spacing: 6
                                // A few sample icons rendered in THIS pack, so
                                // the chip previews the look, not just names it.
                                Repeater {
                                    model: ["transcribe", "chapters", "search"]
                                    AppIcon {
                                        required property string modelData
                                        name: modelData
                                        iconSize: 14
                                        color: Theme.textPrimary
                                        // Preview THIS chip's pack, not the
                                        // active one.
                                        packOverride: packChip.modelData
                                    }
                                }
                                Label {
                                    text: IconTheme.packName(packChip.modelData)
                                    color: packChip.current ? "white" : "#bbbbbb"
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                }
                SettingResetButton { propertyName: "iconPack" }
            }

            // Per-icon overrides — collapsible. Each cell click-cycles that
            // one icon through auto -> each pack -> auto (see
            // cycleIconOverride). The label under the icon shows where it
            // currently comes from ("auto" = the default pack above).
            RowLayout {
                Layout.fillWidth: true
                Item {
                    id: overridesDisclosure
                    implicitWidth: overridesDisclosureRow.implicitWidth
                    implicitHeight: 24
                    HoverHandler { id: overridesDisclosureHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: settingsPopup.iconOverridesExpanded = !settingsPopup.iconOverridesExpanded }
                    RowLayout {
                        id: overridesDisclosureRow
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        AppIcon {
                            name: settingsPopup.iconOverridesExpanded ? "chapter-collapse" : "chapter-expand"
                            iconSize: 12
                            color: overridesDisclosureHover.hovered ? Theme.textPrimary : Theme.textMuted
                        }
                        Label {
                            text: {
                                const n = Object.keys(AppSettings.iconOverrides).length
                                return n > 0 ? qsTr("Per-icon overrides (%1)").arg(n) : qsTr("Per-icon overrides")
                            }
                            color: overridesDisclosureHover.hovered ? Theme.textPrimary : Theme.textMuted
                            font.pixelSize: 12
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                // Clear-all shortcut, only when at least one override is set.
                Label {
                    visible: Object.keys(AppSettings.iconOverrides).length > 0
                    text: qsTr("Reset all")
                    color: clearOverridesHover.hovered ? Theme.danger : Theme.textSecondary
                    font.pixelSize: 11
                    HoverHandler { id: clearOverridesHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: AppSettings.iconOverrides = ({}) }
                }
            }

            Flow {
                Layout.fillWidth: true
                visible: settingsPopup.iconOverridesExpanded
                spacing: 8
                Repeater {
                    model: IconTheme.iconNames()
                    Item {
                        id: overrideCell
                        required property string modelData
                        readonly property string source: AppSettings.iconOverrides[overrideCell.modelData] || ""
                        implicitWidth: 76
                        implicitHeight: 58

                        ToolTip.visible: overrideCellHover.hovered
                        ToolTip.text: qsTr("%1 — from %2. Click to change pack.")
                                      .arg(overrideCell.modelData)
                                      .arg(overrideCell.source === "" ? qsTr("default pack") : overrideCell.source)
                        ToolTip.delay: 500

                        HoverHandler { id: overrideCellHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: settingsPopup.cycleIconOverride(overrideCell.modelData) }

                        Rectangle {
                            anchors.fill: parent
                            radius: AppSettings.borderRadius
                            color: overrideCellHover.hovered ? Theme.surfaceHover : Theme.surface
                            border.width: 1
                            // Overridden cells get an accent border so the
                            // handful in a large grid are easy to spot.
                            border.color: overrideCell.source === "" ? Theme.border : AppSettings.accentColor
                        }
                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 3
                            AppIcon {
                                Layout.alignment: Qt.AlignHCenter
                                name: overrideCell.modelData
                                iconSize: 16
                                color: Theme.textPrimary
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.maximumWidth: 68
                                text: overrideCell.modelData
                                color: "#999999"
                                font.pixelSize: 8
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: overrideCell.source === "" ? qsTr("auto") : overrideCell.source
                                color: overrideCell.source === "" ? "#777777" : AppSettings.accentColor
                                font.pixelSize: 8
                            }
                        }
                    }
                }
            }

            SliderSetting {
                id: radiusSlider
                label: qsTr("Border radius")
                propertyName: "borderRadius"
                to: 24
            }

            ToggleSetting {
                id: titlebarSwitch
                label: qsTr("Use system titlebar")
                propertyName: "useSystemTitlebar"
            }

            }

            // ===== Page 1: Sidebar section =====
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 1

            Label {
                text: qsTr("Sidebar")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Background color")
                }
                Button {
                    id: sidebarColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: sidebarColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (sidebarColorButton.hovered && sidebarColorButton.enabled) ? Theme.surfaceHover : Theme.surface
                        border.width: 1
                        border.color: Theme.border
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarColorButton.hovered
                    }
                }
                SettingResetButton { propertyName: "sidebarBackgroundColor" }
            }

            SliderSetting {
                id: sidebarOpacitySlider
                label: qsTr("Opacity")
                propertyName: "sidebarOpacity"
                to: 1
                decimals: 2
            }

            ToggleSetting {
                id: sidebarBlurSwitch
                label: qsTr("Blur")
                propertyName: "sidebarBlur"
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Selection fill color")
                }
                Button {
                    id: sidebarSelectionColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: sidebarSelectionColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (sidebarSelectionColorButton.hovered && sidebarSelectionColorButton.enabled) ? Theme.surfaceHover : Theme.surface
                        border.width: 1
                        border.color: Theme.border
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarSelectionColorButton.hovered
                    }
                }
                SettingResetButton { propertyName: "sidebarSelectionColor" }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Selection border color")
                }
                Button {
                    id: sidebarSelectionBorderColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: sidebarSelectionBorderColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (sidebarSelectionBorderColorButton.hovered && sidebarSelectionBorderColorButton.enabled) ? Theme.surfaceHover : Theme.surface
                        border.width: 1
                        border.color: Theme.border
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarSelectionBorderColorButton.hovered
                    }
                }
                SettingResetButton { propertyName: "sidebarSelectionBorderColor" }
            }

            SliderSetting {
                id: sidebarEdgeLeftMarginSlider
                label: qsTr("Left edge margin")
                propertyName: "sidebarEdgeLeftMargin"
                to: 20
            }

            SliderSetting {
                id: sidebarContentLeftMarginSlider
                label: qsTr("Left content margin")
                propertyName: "sidebarContentLeftMargin"
                to: 20
            }

            SliderSetting {
                id: sidebarRightMarginSlider
                label: qsTr("Right margin")
                propertyName: "sidebarRightMargin"
                to: 40
            }

            }

            // Main panel section (page 1)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 1

            Label {
                text: qsTr("Main panel")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Background color")
                }
                Button {
                    id: mainColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: mainColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (mainColorButton.hovered && mainColorButton.enabled) ? Theme.surfaceHover : Theme.surface
                        border.width: 1
                        border.color: Theme.border
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainColorButton.hovered
                    }
                }
                SettingResetButton { propertyName: "mainBackgroundColor" }
            }

            SliderSetting {
                id: mainOpacitySlider
                label: qsTr("Opacity")
                propertyName: "mainOpacity"
                to: 1
                decimals: 2
            }

            ToggleSetting {
                id: mainBlurSwitch
                label: qsTr("Blur")
                propertyName: "mainBlur"
            }

            }

            // Rim section (page 0)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 0

            Label {
                text: qsTr("Rim")
                color: "white"
                font.bold: true
            }

            ToggleSetting {
                id: rimEnabledSwitch
                label: qsTr("Enabled")
                propertyName: "rimEnabled"
            }

            SliderSetting {
                id: rimWidthSlider
                label: qsTr("Width")
                propertyName: "rimWidth"
                to: 8
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Color")
                }
                Button {
                    id: rimColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: rimColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (rimColorButton.hovered && rimColorButton.enabled) ? Theme.surfaceHover : Theme.surface
                        border.width: 1
                        border.color: Theme.border
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimColorButton.hovered
                    }
                }
                SettingResetButton { propertyName: "rimColor" }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Recording indicator")
                }
                Button {
                    id: recordingActiveColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: recordingActiveColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (recordingActiveColorButton.hovered && recordingActiveColorButton.enabled) ? Theme.surfaceHover : Theme.surface
                        border.width: 1
                        border.color: Theme.border
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: recordingActiveColorButton.hovered
                    }
                }
                SettingResetButton { propertyName: "recordingActiveColor" }
            }

            }

            // --- Recorder waveform ---
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 3

            Label {
                text: qsTr("Recorder Waveform")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    Layout.alignment: Qt.AlignVCenter
                    wrapMode: Text.WordWrap
                    color: "white"
                    text: qsTr("Waveform")
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: [
                            { id: "filled", label: qsTr("Filled") },
                            { id: "line", label: qsTr("Line") }
                        ]
                        Item {
                            id: waveStyleChip
                            required property var modelData
                            readonly property bool current: AppSettings.recordingWaveformStyle === waveStyleChip.modelData.id
                            implicitWidth: waveStyleChipLabel.implicitWidth + 20
                            implicitHeight: 30

                            HoverHandler { id: waveStyleChipHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: AppSettings.recordingWaveformStyle = waveStyleChip.modelData.id }

                            Rectangle {
                                anchors.fill: parent
                                radius: AppSettings.borderRadius
                                color: waveStyleChip.current ? "#33ffffff"
                                       : (waveStyleChipHover.hovered ? Theme.surfaceHover : Theme.surface)
                                border.width: 1
                                border.color: waveStyleChip.current ? AppSettings.accentColor : Theme.border
                            }
                            Label {
                                id: waveStyleChipLabel
                                anchors.centerIn: parent
                                text: waveStyleChip.modelData.label
                                color: waveStyleChip.current ? "white" : "#bbbbbb"
                                font.pixelSize: 12
                            }
                        }
                    }
                }
                SettingResetButton { propertyName: "recordingWaveformStyle" }
            }

            ToggleSetting {
                id: recordingWaveformMirroredToggle
                label: qsTr("Mirror the wave above and below the centre line")
                labelWrap: true
                propertyName: "recordingWaveformMirrored"
            }

            }

            // --- Recording file format ---
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 3

            Label {
                text: qsTr("Recordings")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    color: "white"
                    text: qsTr("Save takes as")
                }
                AppComboBox {
                    id: recordingFormatCombo
                    objectName: "recordingFormatCombo"
                    Layout.preferredWidth: 180
                    model: AppSettings.recordingFormatOptions()
                    selectedId: AppSettings.recordingFormat
                    onPicked: id => AppSettings.recordingFormat = id
                }
                SettingResetButton { propertyName: "recordingFormat" }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: 11
                text: qsTr("FLAC voice ≈ 70 MB/hour — exactly what transcription uses, so transcripts are "
                           + "unchanged; playback is mono and loses highs. FLAC ≈ 250 MB/hour, full quality. "
                           + "WAV ≈ 690 MB/hour. Takes are encoded as they are recorded; the compress button "
                           + "turns existing WAV or full-quality FLAC files into FLAC voice.")
            }

            }

            // Accent Color section (page 0)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 0

            // Shared accent color for hover outlines (HoverBorder.qml) and
            // the always-editable-field focus underline
            // (UnderlineField.qml) — both were hardcoded "white" before.
            Label {
                text: qsTr("Accent Color")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Color")
                }
                Button {
                    id: accentColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: accentColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (accentColorButton.hovered && accentColorButton.enabled) ? Theme.surfaceHover : Theme.surface
                        border.width: 1
                        border.color: Theme.border
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: accentColorButton.hovered
                    }
                }
                SettingResetButton { propertyName: "accentColor" }
            }

            }

            // Sentence Highlight section (page 0)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 0

            Label {
                text: qsTr("Sentence Highlight")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Color")
                }
                Button {
                    id: highlightColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: highlightColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (highlightColorButton.hovered && highlightColorButton.enabled) ? Theme.surfaceHover : Theme.surface
                        border.width: 1
                        border.color: Theme.border
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: highlightColorButton.hovered
                    }
                }
                SettingResetButton { propertyName: "highlightColor" }
            }

            // Playback + Ctrl+F search-match highlight radius,
            // now independently adjustable from the app-wide border radius
            // above (was previously hardcoded to follow it).
            SliderSetting {
                id: highlightRadiusSlider
                label: qsTr("Radius")
                propertyName: "highlightRadius"
                to: 24
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Applies to the playback sentence highlight and the Ctrl+F search-match highlight.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            }

            // Progress Glow section (page 0)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 0

            Label {
                text: qsTr("Progress Glow")
                color: "white"
                font.bold: true
            }

            ToggleSetting {
                id: progressGlowEnabledSwitch
                label: qsTr("Enabled")
                propertyName: "progressGlowEnabled"
            }

            Label {
                visible: progressGlowEnabledSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Replaces the bottom info bar's progress bar with a thin glowing line across its top edge.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                visible: progressGlowEnabledSwitch.checked
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Color")
                }
                Button {
                    id: progressGlowColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: progressGlowColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (progressGlowColorButton.hovered && progressGlowColorButton.enabled) ? Theme.surfaceHover : Theme.surface
                        border.width: 1
                        border.color: Theme.border
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowColorButton.hovered
                    }
                }
                SettingResetButton { propertyName: "progressGlowColor" }
            }

            SliderSetting {
                id: progressGlowWidthSlider
                visible: progressGlowEnabledSwitch.checked
                label: qsTr("Width")
                propertyName: "progressGlowWidth"
                to: 8
            }

            SliderSetting {
                id: progressGlowRadiusSlider
                visible: progressGlowEnabledSwitch.checked
                label: qsTr("Glow radius")
                propertyName: "progressGlowRadius"
                to: 32
            }

            SliderSetting {
                id: progressGlowLayerCountSlider
                visible: progressGlowEnabledSwitch.checked
                label: qsTr("Layers")
                propertyName: "progressGlowLayerCount"
                from: 1
                to: 5
                stepSize: 1
                decimals: 0
            }

            ToggleSetting {
                id: progressGlowHidesBarSwitch
                visible: progressGlowEnabledSwitch.checked
                label: qsTr("Hide standard progress bar")
                propertyName: "progressGlowHidesBar"
            }

            Label {
                visible: progressGlowEnabledSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("When on, the standard progress bar is hidden while the glow is active. Turn off to show both.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            }

            // Bottom bar section (page 0)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 0

                Label {
                    text: qsTr("Bottom bar")
                    color: "white"
                    font.bold: true
                }
                ToggleSetting {
                    label: qsTr("Click running-task text to open its file")
                    propertyName: "bottomBarTaskClickOpensFile"
                }
                ToggleSetting {
                    label: qsTr("Show full task text on hover")
                    propertyName: "bottomBarTaskTooltip"
                }
                ToggleSetting {
                    label: qsTr("Shrink the vault path to fit its content")
                    propertyName: "bottomBarPathShrinkToContent"
                }
                ToggleSetting {
                    label: qsTr("Shrink/wrap task text instead of truncating")
                    propertyName: "bottomBarTaskTextFit"
                }
            }

            // ===== Page 1 (cont.): Timestamps section =====
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 1

            Label {
                text: qsTr("Timestamps")
                color: "white"
                font.bold: true
            }

            SliderSetting {
                id: timestampDecimalPlacesSlider
                label: qsTr("Decimal places")
                propertyName: "timestampDecimalPlaces"
                to: 3
                stepSize: 1
                decimals: 0
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Rounds the transcript gutter's displayed timestamp only — the underlying data always keeps full precision.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            }

            // ===== Page 2: Files section =====
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 2

            Label {
                text: qsTr("Files")
                color: "white"
                font.bold: true
            }

            ToggleSetting {
                id: useLabelsMainPanelSwitch
                label: qsTr("Show labels instead of filenames (main panel)")
                propertyName: "useLabelsMainPanel"
            }

            Label {
                visible: !useLabelsMainPanelSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Off: renaming a file in the main panel renames the actual audio + transcript files instead of just setting a label.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            ToggleSetting {
                id: useLabelsSidebarSwitch
                label: qsTr("Show labels instead of filenames (sidebar)")
                propertyName: "useLabelsSidebar"
            }

            ToggleSetting {
                id: hideExtensionsSidebarSwitch
                label: qsTr("Hide file extensions (sidebar)")
                propertyName: "hideExtensionsSidebar"
            }

            ToggleSetting {
                id: hideExtensionsMainPanelSwitch
                label: qsTr("Hide file extensions (main panel)")
                propertyName: "hideExtensionsMainPanel"
            }

            }

            // Speaker detection (diarization) section (page 2)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 2

            Label {
                text: qsTr("Speaker detection")
                color: "white"
                font.bold: true
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Which models find and distinguish speakers. Chosen models download on "
                           + "first use; manage them under Models. Far-field group recordings are hard — "
                           + "try a different embedding model and a lower sensitivity if everyone collapses "
                           + "into one speaker.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    Layout.alignment: Qt.AlignVCenter
                    wrapMode: Text.WordWrap
                    color: "white"
                    text: qsTr("Segmentation")
                }
                ComboBox {
                    id: segmentationModelCombo
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    model: transcriptionController.availableSegmentationModels()
                    textRole: "label"
                    valueRole: "id"
                    // See ModelComboBox.qml: touch `count` so this re-runs when
                    // the list populates (indexOfValue isn't a tracked dep), and
                    // default to the first entry instead of an empty box.
                    currentIndex: {
                        const n = count
                        const i = indexOfValue(AppSettings.diarizationSegmentationModel)
                        return i >= 0 ? i : (n > 0 ? 0 : -1)
                    }
                    hoverEnabled: true
                    onActivated: AppSettings.diarizationSegmentationModel = currentValue

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: segmentationModelCombo.hovered
                    }
                }
                SettingResetButton { propertyName: "diarizationSegmentationModel" }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    Layout.alignment: Qt.AlignVCenter
                    wrapMode: Text.WordWrap
                    color: "white"
                    text: qsTr("Embedding")
                }
                ComboBox {
                    id: embeddingModelCombo
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    model: transcriptionController.availableEmbeddingModels()
                    textRole: "label"
                    valueRole: "id"
                    // See ModelComboBox.qml: touch `count` so this re-runs when
                    // the list populates (indexOfValue isn't a tracked dep), and
                    // default to the first entry instead of an empty box.
                    currentIndex: {
                        const n = count
                        const i = indexOfValue(AppSettings.diarizationEmbeddingModel)
                        return i >= 0 ? i : (n > 0 ? 0 : -1)
                    }
                    hoverEnabled: true
                    onActivated: AppSettings.diarizationEmbeddingModel = currentValue

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: embeddingModelCombo.hovered
                    }
                }
                SettingResetButton { propertyName: "diarizationEmbeddingModel" }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: segmentationModelCombo.currentValue === "reverb-diarization-v1"
                text: qsTr("⚠ Reverb v1 is released under a non-commercial / research license.")
                color: "#c9a227"
                font.pixelSize: 11
            }

            SliderSetting {
                id: diarizationSensitivitySlider
                label: qsTr("Sensitivity")
                propertyName: "diarizationSensitivity"
                // Continuous (like chapterDensity) — NOT the 3-tier snap the
                // labeling sliders use. Clamped away from the 0/1 extremes,
                // which degenerate (everything one cluster / every turn its own).
                from: 0.1
                to: 0.9
                showField: false
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Clustering distance cutoff (%1). Lower = stricter = MORE speakers; higher = "
                           + "more merging = FEWER. Tunes auto-detection — it is not a fixed speaker count.")
                        .arg(diarizationSensitivitySlider.value.toFixed(2))
                color: Theme.textDisabled
                font.italic: true
                font.pixelSize: 11
            }

            }

            // AI Auto-Labeling section (page 2)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 2

            Label {
                text: qsTr("AI Auto-Labeling")
                color: "white"
                font.bold: true
            }

            ToggleSetting {
                id: autoLabelEnabledSwitch
                label: qsTr("Auto-generate label, prelude, and tags")
                propertyName: "autoLabelEnabled"
            }

            Label {
                visible: autoLabelEnabledSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Runs a small local AI model (~2.5 GB download on first use) after each "
                           + "transcription to generate a display label, prelude, and tags.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            ToggleSetting {
                id: restrictTagsToExistingSwitch
                visible: autoLabelEnabledSwitch.checked
                label: qsTr("Only use existing tags (auto-categorize)")
                propertyName: "restrictTagsToExisting"
            }

            Label {
                visible: autoLabelEnabledSwitch.checked && restrictTagsToExistingSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Off (default): freely invent new tags. On: only pick from tags already used "
                           + "elsewhere in this vault — leaves a file untagged rather than inventing one "
                           + "if nothing existing fits.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            // Only meaningful when NOT hard-restricted —
            // restrictTagsToExisting above is already a hard rule, so this
            // gradient wouldn't have anything to adjust while it's on.
            SliderSetting {
                id: tagReuseBiasSlider
                visible: autoLabelEnabledSwitch.checked && !restrictTagsToExistingSwitch.checked
                label: qsTr("Tag reuse bias")
                propertyName: "tagReuseBias"
                to: 1
                stepSize: 0.5
                showField: false
            }

            Label {
                visible: autoLabelEnabledSwitch.checked && !restrictTagsToExistingSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("How much to favor reusing an existing tag over inventing a new one. Low: "
                           + "prefer a precise new tag. High: stretch and reuse an existing tag even if "
                           + "it's only a loose fit.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            Label {
                visible: autoLabelEnabledSwitch.checked && !restrictTagsToExistingSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Current prompt: “%1”")
                        .arg(settingsPopup.tagReuseBiasPhrasing(tagReuseBiasSlider.value))
                color: Theme.textDisabled
                font.italic: true
                font.pixelSize: 11
            }

            SliderSetting {
                id: wordinessSlider
                visible: autoLabelEnabledSwitch.checked
                label: qsTr("Label wordiness")
                propertyName: "wordiness"
                to: 1
                stepSize: 0.5
                showField: false
            }

            Label {
                visible: autoLabelEnabledSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("How verbose the AI-generated label and prelude are. Low: terse. High: "
                           + "descriptive.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            Label {
                visible: autoLabelEnabledSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Current prompt: “%1”").arg(settingsPopup.wordinessPhrasing(wordinessSlider.value))
                color: Theme.textDisabled
                font.italic: true
                font.pixelSize: 11
            }

            // Chapter-generation density. Deliberately NOT gated on
            // autoLabelEnabledSwitch (unlike the two sliders above) — chapters
            // are generated on demand via the "Generate chapters" button
            // regardless of whether the auto-label pass is enabled, so this
            // knob must stay reachable even with auto-labeling off. Continuous
            // (no discrete-tier snap) since generateChapters() takes a raw
            // 0.0-1.0 double, not a 3-way prompt tier.
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    Layout.alignment: Qt.AlignVCenter
                    wrapMode: Text.WordWrap
                    color: "white"
                    text: qsTr("Chapter density")
                }
                Label {
                    text: qsTr("fewer")
                    color: Theme.textSecondary
                    font.pixelSize: 10
                }
                Slider {
                    id: chapterDensitySlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 1
                    value: AppSettings.chapterDensity
                    hoverEnabled: true
                    onMoved: AppSettings.chapterDensity = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: chapterDensitySlider.hovered
                    }
                }
                Label {
                    text: qsTr("more")
                    color: Theme.textSecondary
                    font.pixelSize: 10
                }
                SettingResetButton { propertyName: "chapterDensity" }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("How many chapters \"Generate chapters\" produces. Fewer: only major "
                           + "topic shifts. More: finer-grained sections.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            }

            // ===== Page 3: Combine split recordings section =====
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: settingsPopup.currentPage === 4

            Label {
                text: qsTr("Combine split recordings")
                color: "white"
                font.bold: true
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Some recorders split one continuous recording into several files (e.g. a "
                           + "size or time limit). Rules below detect those parts so they can be "
                           + "merged into one file — losslessly, before transcription. Combining runs "
                           + "when you ask for it (the sidebar's combine button, or a group's own "
                           + "button), never on its own as parts arrive.")
                color: Theme.textSecondary
                font.pixelSize: 11
            }

            ToggleSetting {
                id: deleteOriginalsAfterCombineSwitch
                label: qsTr("Delete original parts after combining (instead of moving them to .combined)")
                propertyName: "deleteOriginalsAfterCombine"
                labelWrap: true
            }

            // The ONE automatic combine, and deliberately a narrow one: it runs
            // after a USB auto-grab batch finishes — a discrete moment when a
            // split recording has just finished arriving — not on every vault
            // change (that behavior existed once and was removed for silently
            // merging parts mid-recording; see AppSettings.autoCombineAfterGrab).
            ToggleSetting {
                id: autoCombineAfterGrabSwitch
                label: qsTr("Combine automatically after a USB auto-grab finishes")
                propertyName: "autoCombineAfterGrab"
                labelWrap: true
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Repeater {
                    id: combineRuleRepeater
                    model: AppSettings.combineRules

                    delegate: ColumnLayout {
                        id: ruleCard
                        required property var modelData
                        required property int index

                        property var previewGroups: []
                        property bool previewRun: false
                        property var differentiatorRuns: []

                        Layout.fillWidth: true
                        spacing: 4

                        // Inserts `snippet` at the current cursor position in
                        // rulePatternField, places the cursor `cursorOffset`
                        // characters into the inserted snippet, and refocuses
                        // the field so typing continues immediately. Uses
                        // TextInput.insert() rather than a raw `text =`
                        // reassignment — verified empirically (offscreen
                        // QTest) that reassigning the whole `text` property
                        // silently clears TextInput's undo history, while
                        // insert() records a normal undoable edit, so
                        // Ctrl+Z/Ctrl+Y keep working after clicking one of
                        // these buttons exactly as they do for typed input.
                        // Deliberately does NOT call settingsPopup.updateRule()
                        // itself — the field's own onEditingFinished commits
                        // the change on focus-out/Enter exactly as it does for
                        // manual typing, so insert-then-keep-typing and
                        // insert-then-click-away both commit the same way.
                        // A non-empty selection is removed first so the button
                        // OVERWRITES it rather than inserting alongside it —
                        // remove() is just as undoable as insert() (both are
                        // real TextInput edit operations, unlike a `text =`
                        // reassignment), so Ctrl+Z still works in one step.
                        function insertPatternToken(snippet, cursorOffset) {
                            if (rulePatternField.selectedText.length > 0)
                                rulePatternField.remove(rulePatternField.selectionStart, rulePatternField.selectionEnd)
                            const pos = rulePatternField.cursorPosition
                            rulePatternField.insert(pos, snippet)
                            rulePatternField.cursorPosition = pos + cursorOffset
                            rulePatternField.forceActiveFocus()
                        }

                        // Re-runs the SAME detectCombineGroups() the ribbon
                        // "combine" button and per-job safety-net redirect
                        // use, so the preview below is always exactly what
                        // would really happen with the pattern as currently
                        // typed — not a separate simulation the user has to
                        // remember to re-trigger.
                        function updatePreview() {
                            if (rulePatternField.text.length > 0 && settingsPopup.vaultTreeModel
                                    && settingsPopup.transcriptionController) {
                                // Resolved against the LIVE typed text, not the
                                // committed rule.pattern — the pattern field can
                                // be mid-edit (uncommitted) here, so the sort
                                // differentiator's group index needs re-deriving
                                // the same way commitPatternChange() does on an
                                // actual commit, or a same-session edit that
                                // shifts group numbering would show a stale
                                // preview until the field loses focus.
                                const sortGroupIndex = CombineRuleRegex.resolveSortGroupIndex(
                                            rulePatternField.text, ruleCard.modelData.sortGroupText || "",
                                            ruleCard.modelData.sortGroupOccurrence || 0)
                                const testRule = {name: ruleCard.modelData.name || qsTr("(unnamed)"),
                                                   pattern: rulePatternField.text, enabled: true,
                                                   sortGroupIndex: sortGroupIndex}
                                ruleCard.previewGroups = settingsPopup.transcriptionController.detectCombineGroups(
                                            [testRule], settingsPopup.vaultTreeModel.allAudioPaths())
                                ruleCard.previewRun = true
                            } else {
                                ruleCard.previewGroups = []
                                ruleCard.previewRun = false
                            }
                            ruleCard.updateDifferentiatorRuns()
                        }

                        // Recomputes the differentiator buttons for the
                        // pattern as currently typed, including — per run —
                        // which files (if any) would violate "this
                        // differentiator must participate in every match"
                        // (see TranscriptionController::
                        // differentiatorNonParticipatingFiles()'s doc
                        // comment: a zero-width capture is fine, only a
                        // capture that doesn't participate AT ALL, e.g. it
                        // sits in an alternation branch that wasn't taken,
                        // counts). For an OFF run this decides whether the
                        // button gets disabled (BLOCKED); for an already-ON
                        // run it decides whether a non-blocking warning is
                        // shown (nothing to block — the rule is already
                        // saved this way).
                        function updateDifferentiatorRuns() {
                            const text = rulePatternField.text
                            const runs = CombineRuleRegex.findDifferentiatorRuns(text)
                            const hasBackend = settingsPopup.transcriptionController && settingsPopup.vaultTreeModel
                            const audioPaths = hasBackend ? settingsPopup.vaultTreeModel.allAudioPaths() : []
                            // Disambiguates duplicate-text ON runs (e.g. two
                            // "(\d+)" groups — an entirely realistic pattern,
                            // not a corner case) for the sort-differentiator
                            // designation: "the Nth ON run, left to right,
                            // with this exact text" — see
                            // resolveSortGroupIndex()'s doc comment, which
                            // this counting must stay in lockstep with (same
                            // isDifferentiator filter, same left-to-right
                            // order).
                            const occurrenceByText = {}
                            for (const run of runs) {
                                // groupIndex is the run's REAL (if already ON) or
                                // HYPOTHETICAL (if OFF, as if just toggled ON)
                                // capture-group number — cheap pure-text derivation,
                                // computed unconditionally so an ON button can color
                                // itself to match its differentiatorColor() preview
                                // span even when there's no backend/vault to run the
                                // participation check against.
                                const candidatePattern = run.isDifferentiator
                                        ? text : CombineRuleRegex.toggleDifferentiatorRun(text, run)
                                run.groupIndex = CombineRuleRegex.capturingGroupIndexAt(candidatePattern, run.start)
                                run.offendingFiles = (hasBackend && run.groupIndex > 0)
                                        ? settingsPopup.transcriptionController.differentiatorNonParticipatingFiles(
                                              candidatePattern, run.groupIndex, audioPaths)
                                        : []
                                if (run.isDifferentiator) {
                                    run.occurrence = occurrenceByText[run.text] || 0
                                    occurrenceByText[run.text] = run.occurrence + 1
                                } else {
                                    run.occurrence = 0
                                }
                            }
                            ruleCard.differentiatorRuns = runs
                        }

                        // Direct-commit toggle (like removeRuleButton /
                        // ruleEnabledSwitch above, NOT insertPatternToken's
                        // rely-on-the-field's-own-commit pattern) — this
                        // button's own click fully reassigns
                        // AppSettings.combineRules itself, synchronously,
                        // before any Repeater rebuild can happen, so it
                        // doesn't hit the focus-steal race described in the
                        // insert-token toolbar's doc comment below and
                        // doesn't need focusPolicy: Qt.NoFocus.
                        //
                        // Three-state cycle per run: OFF (plain text) ->
                        // GROUPING (capturing, contributes to the group
                        // identity key) -> SORT (still capturing, but
                        // EXCLUDED from the key — orders a group's members
                        // by this value instead, numeric-aware; see
                        // findCombineGroups()'s doc comment) -> back to OFF.
                        // Only one run per rule can be in the SORT state at
                        // a time — landing on it here always REPLACES
                        // whatever the rule's previous sort differentiator
                        // was, satisfying "a sort differentiator can't also
                        // be a grouping one" by construction rather than as
                        // a separate validation step.
                        function toggleDifferentiator(run) {
                            // Must compare occurrence too, not just text —
                            // two runs can share identical text (e.g. two
                            // "(\d+)" groups), and text alone can't tell
                            // which specific one is currently the sort field.
                            const isSortRun = run.isDifferentiator && ruleCard.modelData.sortGroupText === run.text
                                    && (ruleCard.modelData.sortGroupOccurrence || 0) === run.occurrence
                            if (isSortRun) {
                                // SORT -> OFF
                                const newPattern = CombineRuleRegex.toggleDifferentiatorRun(rulePatternField.text, run)
                                rulePatternField.text = newPattern
                                settingsPopup.commitPatternChange(ruleCard.index, newPattern, "")
                            } else if (run.isDifferentiator) {
                                // GROUPING -> SORT (pattern text itself is
                                // unchanged — only the sort designation moves)
                                settingsPopup.commitPatternChange(ruleCard.index, rulePatternField.text, run.text, run.occurrence)
                            } else {
                                // OFF -> GROUPING
                                const newPattern = CombineRuleRegex.toggleDifferentiatorRun(rulePatternField.text, run)
                                rulePatternField.text = newPattern
                                settingsPopup.commitPatternChange(ruleCard.index, newPattern)
                            }
                        }

                        // Debounced so the preview isn't recomputed on every
                        // single keystroke — coalesces into one scan shortly
                        // after typing settles, mirroring AppSettings' own
                        // scheduleSave() debounce pattern.
                        Timer {
                            id: previewTimer
                            interval: 300
                            onTriggered: ruleCard.updatePreview()
                        }

                        Component.onCompleted: ruleCard.updatePreview()

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: Theme.border
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            TextField {
                                id: ruleNameField
                                Layout.preferredWidth: 130
                                text: ruleCard.modelData.name
                                placeholderText: qsTr("Rule name")
                                hoverEnabled: true
                                background: UnderlineField { focused: ruleNameField.activeFocus }
                                onEditingFinished: settingsPopup.updateRule(ruleCard.index, "name", text)
                            }
                            Item { Layout.fillWidth: true }
                            Switch {
                                id: ruleEnabledSwitch
                                checked: ruleCard.modelData.enabled
                                hoverEnabled: true
                                onToggled: settingsPopup.updateRule(ruleCard.index, "enabled", checked)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: ruleEnabledSwitch.hovered }
                            }
                            Button {
                                id: removeRuleButton
                                text: qsTr("Remove")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Theme.border
                                }
                                hoverEnabled: true
                                onClicked: settingsPopup.removeRule(ruleCard.index)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: removeRuleButton.hovered }
                            }
                        }

                        TextField {
                            id: rulePatternField
                            Layout.fillWidth: true
                            text: ruleCard.modelData.pattern
                            placeholderText: qsTr("^(TX\\d{2})_MIC\\d{3}_.*_orig\\.wav$")
                            font.family: "monospace"
                            hoverEnabled: true
                            background: UnderlineField { focused: rulePatternField.activeFocus }
                            onEditingFinished: settingsPopup.commitPatternChange(ruleCard.index, text)
                            onTextChanged: previewTimer.restart()
                        }

                        // Was a non-modal Popup reparented onto the
                        // small "ⓘ" info button (regexInfoButton), positioned
                        // relative to THAT button rather than this popup's own
                        // root — near an edge, or after the rule list scrolls,
                        // it could render partially/fully outside
                        // settingsPopup's own bounds, and being non-modal it
                        // never dimmed/blocked the rule-card text behind it.
                        // Replaced with plain always-visible inline text
                        // directly under the pattern field, so it can never
                        // escape this card's own layout.
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textSecondary
                            font.pixelSize: 11
                            text: qsTr("Regex quick reference: . any char · * + ? repeat (0-or-more / "
                                       + "1-or-more / 0-or-1) · ^ $ start/end · \\d \\w \\s digit/word/space "
                                       + "· {n} {n,m} repeat count · [...] any one of these · (...) capture "
                                       + "group — must match IDENTICALLY across files to combine, everything "
                                       + "else may vary freely. | (alternation) has the LOWEST precedence of "
                                       + "any operator — always group it: (?:a|b).")
                        }

                        // Live validity indicator — validates against the
                        // EXACT SAME QRegularExpression engine
                        // findCombineGroups()/matchRuleCapturedKey() actually
                        // use (TranscriptionController::regexValidationError()),
                        // not QML/JS RegExp, whose syntax isn't guaranteed
                        // identical. Reactive off rulePatternField.text — this
                        // is a plain Popup Repeater delegate, not a recycled
                        // TreeView row, so a keystroke-reactive binding is
                        // correct here (no explicit-push idiom needed).
                        Label {
                            id: patternValidityLabel
                            Layout.fillWidth: true
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            visible: rulePatternField.text.length > 0 && settingsPopup.transcriptionController
                            property string validationError: settingsPopup.transcriptionController
                                    ? settingsPopup.transcriptionController.regexValidationError(rulePatternField.text)
                                    : ""
                            color: validationError.length > 0 ? "#e0664f" : "#5fbf7a"
                            text: validationError.length > 0
                                    ? ("✗ " + validationError)
                                    : ("✓ " + qsTr("Valid"))
                            HoverHandler { id: patternValidityHover }
                            ToolTip.visible: patternValidityLabel.validationError.length > 0 && patternValidityHover.hovered
                            ToolTip.text: patternValidityLabel.validationError
                        }

                        // Insert-token toolbar: each button inserts a snippet
                        // at the cursor rather than committing directly, so
                        // typing/editing-finished behaves exactly as if the
                        // user had typed it themselves. "Either/Or" in
                        // particular exists so alternation is never typed
                        // ungrouped in the first place (see
                        // hasUngroupedAlternation()'s doc comment above).
                        // focusPolicy: Qt.NoFocus on every button below is
                        // required, not decorative: a click's press phase
                        // steals rulePatternField's active focus before the
                        // click's release phase fires onClicked, which fires
                        // rulePatternField's onEditingFinished, which commits
                        // via settingsPopup.updateRule() — that reassigns the
                        // WHOLE AppSettings.combineRules array, so the
                        // Repeater destroys and recreates every rule card
                        // (including the very button being pressed) before
                        // "clicked" can fire. Without NoFocus, insertion only
                        // succeeds on alternating clicks: the one after an
                        // insertion has divergent uncommitted text and steals
                        // focus, destroying the button mid-click; the next
                        // click (nothing to commit) succeeds normally.
                        Flow {
                            Layout.fillWidth: true
                            spacing: 4

                            Button {
                                id: insertDigitsButton
                                text: qsTr("Digits")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Theme.border
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken("\\d+", 3)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertDigitsButton.hovered }
                            }
                            Button {
                                id: insertAnyButton
                                text: qsTr("Any")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Theme.border
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken(".*?", 3)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertAnyButton.hovered }
                            }
                            Button {
                                id: insertGroupButton
                                text: qsTr("Group")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Theme.border
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken("()", 1)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertGroupButton.hovered }
                            }
                            Button {
                                id: insertEitherOrButton
                                text: qsTr("Either/Or")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Theme.border
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken("(?:|)", 3)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertEitherOrButton.hovered }
                            }
                            Button {
                                id: insertStartButton
                                text: qsTr("Start")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Theme.border
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken("^", 1)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertStartButton.hovered }
                            }
                            Button {
                                id: insertEndButton
                                text: qsTr("End")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Theme.border
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken("$", 1)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertEndButton.hovered }
                            }
                        }

                        // Ungrouped-alternation footgun warning — the single
                        // highest-value guard here: `|` is the lowest-
                        // precedence regex operator, so an ungrouped one
                        // silently splits the WHOLE pattern into unrelated
                        // top-level alternatives instead of choosing between
                        // two local tokens. Only shown once the pattern is
                        // otherwise syntactically valid (an invalid pattern
                        // already gets its own error above).
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 11
                            color: "#e8a33d"
                            visible: rulePatternField.text.length > 0
                                     && patternValidityLabel.validationError.length === 0
                                     && CombineRuleRegex.hasUngroupedAlternation(rulePatternField.text)
                            text: qsTr("This \"|\" isn't inside its own group, so it may match far more "
                                       + "than intended — e.g. \"^a_|b$\" really means \"start with a_\" OR "
                                       + "\"contains b$\" as two totally separate patterns, not \"a_ followed "
                                       + "by either b\". Wrap the alternatives: (?:a|b).")
                        }

                        // Rule-list-level overlap warning — non-blocking (see
                        // computeRuleOverlaps()'s doc comment: general regex
                        // disjointness is undecidable, this only catches
                        // identical-pattern-text and files that empirically
                        // match more than one enabled rule right now).
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 11
                            color: "#e8a33d"
                            property var overlaps: settingsPopup.combineOverlapWarnings[ruleCard.modelData.name] || []
                            visible: overlaps.length > 0
                            text: {
                                if (overlaps.length === 0)
                                    return ""
                                const withFile = overlaps.filter(function (o) { return o.file.length > 0 })
                                const identical = overlaps.filter(function (o) { return o.file.length === 0 })
                                const parts = []
                                if (identical.length > 0)
                                    parts.push(qsTr("Same pattern as: %1").arg(
                                            identical.map(function (o) { return o.other }).join(", ")))
                                if (withFile.length > 0) {
                                    const shown = withFile.slice(0, 3).map(function (o) {
                                        return o.other + " (" + o.file + ")"
                                    }).join(", ")
                                    const extra = withFile.length > 3 ? qsTr(" +%1 more").arg(withFile.length - 3) : ""
                                    parts.push(qsTr("Also matches the same file(s) as: %1%2").arg(shown).arg(extra))
                                }
                                return "⚠ " + parts.join(" ")
                            }
                        }

                        // Differentiator buttons: one per non-exact-match
                        // ("special") run of the pattern as currently typed
                        // (see findDifferentiatorRuns()'s doc comment above).
                        // Clicking toggles that run between a plain/
                        // non-capturing match (still required for the file
                        // to be recognized as part of a sequential group,
                        // but doesn't affect WHICH group) and a real
                        // capturing group (its captured text must be
                        // identical across files for them to combine — see
                        // matchRuleCapturedKey() in
                        // TranscriptionController.cpp). Direct-commit click
                        // handler (see ruleCard.toggleDifferentiator()'s doc
                        // comment) — no focusPolicy: Qt.NoFocus needed here,
                        // unlike the insert-token toolbar below.
                        Flow {
                            Layout.fillWidth: true
                            spacing: 4
                            visible: ruleCard.differentiatorRuns.length > 0

                            Repeater {
                                model: ruleCard.differentiatorRuns
                                delegate: Button {
                                    id: differentiatorButton
                                    required property var modelData
                                    readonly property bool blocked: !modelData.isDifferentiator
                                            && modelData.offendingFiles.length > 0
                                    // The third state of the OFF -> GROUPING ->
                                    // SORT -> OFF cycle (see toggleDifferentiator()'s
                                    // doc comment) — this run currently orders its
                                    // group's files instead of identifying the
                                    // group, so it deliberately does NOT get a
                                    // differentiatorColor() (that palette means
                                    // "same color = same group," which no longer
                                    // applies once a run stops contributing to the
                                    // key).
                                    // Occurrence, not just text, must match —
                                    // two runs can share identical text (e.g.
                                    // two "(\d+)" groups), and only ONE of
                                    // them is really the sort field.
                                    readonly property bool isSortRun: ruleCard.modelData.sortGroupText === modelData.text
                                            && (ruleCard.modelData.sortGroupOccurrence || 0) === modelData.occurrence
                                    // The button's own highlight uses the SAME
                                    // per-group color classifyFilenameHtml() paints
                                    // that differentiator's text with in the preview
                                    // below — pairs each color to the regex snippet
                                    // that produced it (see differentiatorPalette's
                                    // doc comment: this pairing is the accessibility
                                    // mitigation for the palette's CVD floor-band
                                    // warning, not just a nice-to-have). Qt.darker()/
                                    // Qt.lighter() derive the fill/hover states from
                                    // that one base color — the only place this file
                                    // computes color variants instead of hand-picking
                                    // hex per state, because 8 palette slots x 3
                                    // states each isn't worth hand-tuning.
                                    readonly property color baseColor: differentiatorButton.isSortRun
                                            ? Theme.textSecondary : CombineRuleRegex.differentiatorColor(modelData.groupIndex)
                                    text: (differentiatorButton.isSortRun ? "⇅ " : (modelData.isDifferentiator ? "✓ " : "")) + modelData.text
                                    implicitHeight: 22
                                    enabled: !blocked
                                    background: Rectangle {
                                        radius: AppSettings.borderRadius
                                        color: differentiatorButton.modelData.isDifferentiator
                                                ? Qt.darker(differentiatorButton.baseColor, differentiatorButton.hovered ? 1.7 : 2.4)
                                                : (differentiatorButton.hovered && !differentiatorButton.blocked
                                                   ? Theme.surfaceHover : Theme.surface)
                                        border.width: 1
                                        border.color: differentiatorButton.modelData.isDifferentiator
                                                ? differentiatorButton.baseColor : Theme.border
                                        opacity: differentiatorButton.blocked ? 0.5 : 1
                                    }
                                    font.family: "monospace"
                                    font.pixelSize: 11
                                    hoverEnabled: true
                                    onClicked: ruleCard.toggleDifferentiator(modelData)
                                    HoverHandler {
                                        id: differentiatorHover
                                        cursorShape: differentiatorButton.blocked ? Qt.ArrowCursor : Qt.PointingHandCursor
                                    }
                                    HoverBorder { anchors.fill: parent; active: differentiatorHover.hovered && !differentiatorButton.blocked }
                                    ToolTip.visible: differentiatorHover.hovered
                                    ToolTip.text: {
                                        if (differentiatorButton.blocked) {
                                            const shown = modelData.offendingFiles.slice(0, 3).join(", ")
                                            const extra = modelData.offendingFiles.length > 3
                                                    ? qsTr(" (+%1 more)").arg(modelData.offendingFiles.length - 3) : ""
                                            return qsTr("Can't make this a differentiator — it doesn't appear in "
                                                        + "some matching files at all: %1%2").arg(shown).arg(extra)
                                        }
                                        if (modelData.isDifferentiator && modelData.offendingFiles.length > 0) {
                                            const shown = modelData.offendingFiles.slice(0, 3).join(", ")
                                            return qsTr("Warning: doesn't appear in some matching files: %1").arg(shown)
                                        }
                                        if (differentiatorButton.isSortRun)
                                            return qsTr("Sorts combined files by this value (numeric if every "
                                                        + "matched file has a numeric one here, otherwise "
                                                        + "alphabetical) instead of grouping by it. Click to turn off.")
                                        if (modelData.isDifferentiator)
                                            return qsTr("Differentiator — must match identically for files to "
                                                        + "combine. Click to sort combined files by this instead.")
                                        return qsTr("Click to make this a differentiator")
                                    }
                                }
                            }
                        }

                        // Non-blocking warning for an already-ON
                        // differentiator that doesn't participate in every
                        // currently-matched file — can't block a click that
                        // already happened / a rule that's already saved,
                        // so this is advisory only (see
                        // updateDifferentiatorRuns()'s doc comment).
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 11
                            color: "#e8a33d"
                            property var brokenOnRuns: ruleCard.differentiatorRuns.filter(function (r) {
                                return r.isDifferentiator && r.offendingFiles.length > 0
                            })
                            visible: brokenOnRuns.length > 0
                            text: brokenOnRuns.length > 0
                                    ? qsTr("⚠ Differentiator \"%1\" doesn't appear in some matching files: %2")
                                          .arg(brokenOnRuns[0].text)
                                          .arg(brokenOnRuns[0].offendingFiles.slice(0, 3).join(", "))
                                    : ""
                        }

                        // Live syntax-highlight readout — reflects every
                        // keystroke in rulePatternField directly (not the
                        // committed modelData.pattern), same "see it as you
                        // type" intent as the regex quick-reference text above.
                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.RichText
                            wrapMode: Text.Wrap
                            font.family: "monospace"
                            font.pixelSize: 11
                            text: CombineRuleRegex.highlightPattern(rulePatternField.text)
                            visible: rulePatternField.text.length > 0
                        }

                        Label {
                            visible: ruleCard.previewRun && ruleCard.previewGroups.length === 0
                            text: qsTr("No files in this vault match.")
                            color: Theme.textSecondary
                            font.pixelSize: 11
                        }

                        // Preview cards: one per detected group, showing the
                        // combine order and what would happen — this is the
                        // SAME detectCombineGroups() the ribbon "combine"
                        // button and the per-job safety-net redirect both
                        // use, so what's shown here is exactly what would
                        // really happen, not a separate simulation.
                        Repeater {
                            model: ruleCard.previewRun ? ruleCard.previewGroups : []
                            delegate: Rectangle {
                                id: previewCard
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.preferredHeight: previewCardColumn.implicitHeight + 12
                                radius: AppSettings.borderRadius
                                color: "#1e1e1e"
                                border.width: 1
                                border.color: Theme.border

                                ColumnLayout {
                                    id: previewCardColumn
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    spacing: 2
                                    // Header: exactly which differentiator
                                    // values grouped these files together —
                                    // color-matched to the differentiator
                                    // toggle buttons and the per-filename
                                    // highlighting below, so "same color
                                    // here as up there" tells you why these
                                    // files landed in the same group without
                                    // reading regex.
                                    TextEdit {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        readOnly: true
                                        selectByMouse: true
                                        selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)
                                        color: Theme.textSecondary
                                        font.pixelSize: 10
                                        font.bold: true
                                        textFormat: Text.RichText
                                        text: {
                                            const values = previewCard.modelData.capturedValues || []
                                            if (values.length === 0)
                                                return qsTr("No differentiators — this rule groups every matching file together")
                                            return qsTr("Differentiators: ") + values.map(function (v, i) {
                                                const shown = v.length > 0 ? CombineRuleRegex.escapeRegexPreviewHtml(v) : qsTr("(empty)")
                                                return "<span style=\"color:" + CombineRuleRegex.differentiatorColor(i + 1) + ";\">"
                                                        + shown + "</span>"
                                            }).join(", ")
                                        }
                                    }
                                    TextEdit {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        readOnly: true
                                        selectByMouse: true
                                        selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)
                                        color: Theme.textPrimary
                                        font.pixelSize: 11
                                        text: {
                                            switch (previewCard.modelData.status) {
                                            case "ready": return qsTr("Ready to combine (%1 parts):").arg(previewCard.modelData.files.length)
                                            case "singleton": return qsTr("Only one matching file — nothing to combine yet.")
                                            case "alreadyTranscribed": return qsTr("Skipped — one of these parts is already transcribed.")
                                            case "alreadyCombined": return qsTr("Already combined into %1.").arg(previewCard.modelData.outputPath)
                                            case "pending": return qsTr("Waiting — the most recent part was modified very recently.")
                                            default: return previewCard.modelData.status
                                            }
                                        }
                                    }
                                    TextEdit {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        visible: previewCard.modelData.files.length > 0
                                        readOnly: true
                                        selectByMouse: true
                                        selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)
                                        color: Theme.textSecondary
                                        font.pixelSize: 10
                                        textFormat: Text.RichText
                                        text: previewCard.modelData.files.map(function (f, i) {
                                            const name = f.substring(f.lastIndexOf("/") + 1)
                                            return (i + 1) + ". " + CombineRuleRegex.classifyFilenameHtml(rulePatternField.text, name)
                                        }).join("<br>")
                                    }
                                    TextEdit {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        visible: previewCard.modelData.status === "ready"
                                        readOnly: true
                                        selectByMouse: true
                                        selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)
                                        color: Theme.textSecondary
                                        font.pixelSize: 10
                                        text: qsTr("Output: %1").arg(
                                                  previewCard.modelData.outputPath.substring(
                                                      previewCard.modelData.outputPath.lastIndexOf("/") + 1))
                                    }
                                }
                            }
                        }
                    }
                }

                Button {
                    id: addRuleButton
                    text: qsTr("+ Add rule")
                    Layout.alignment: Qt.AlignLeft
                    implicitHeight: 24
                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (addRuleButton.hovered ? Theme.surfaceHover : Theme.surface)
                        border.width: 1
                        border.color: Theme.border
                    }
                    hoverEnabled: true
                    onClicked: settingsPopup.addRule()
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    HoverBorder { anchors.fill: parent; active: addRuleButton.hovered }
                }
            }
            }

            // ===== Page 4: Models (ModelsPage.qml) =====
            // Its own component — download/predownload/delete/import/export of
            // the app-global model store (see DESIGN.md "Model management").
            ModelsPage {
                Layout.fillWidth: true
                visible: settingsPopup.currentPage === 5
                transcriptionController: settingsPopup.transcriptionController
            }

            // Benchmark was promoted out of Settings to its own top-level
            // TitleBar page (see MainPanel's BenchmarkPage block) — no longer a
            // settings sub-page.
        }
    }

    // Chrome/Firefox-style middle-click autoscroll for the Settings
    // ScrollView — see MiddleDragScroll.qml for the whole design. A sibling
    // overlay of scrollArea (not inside it) so it isn't counted as the
    // ScrollView's own content; anchored to the same region scrollArea covers
    // (right of the nav rail, below the header chrome) so a middle-click arms
    // it only from the scrollable page content, not from the nav rail or the
    // persistent header.
    MiddleDragScroll {
        id: middleDragScrollArea
        anchors.left: navColumn.right
        anchors.right: parent.right
        anchors.top: headerRow.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: 12
        anchors.topMargin: 10
        anchors.bottomMargin: 16
        anchors.rightMargin: 2
        flickable: scrollArea.contentItem as Flickable
    }
}
