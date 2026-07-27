pragma Singleton
import QtQuick
import QtCore

// Item 9 (Wave E): appearance/behavior settings now live in the VAULT
// (<vault>/.transcriber/settings.json — VaultTreeModel::readVaultSettings/
// writeVaultSettings), not an app-scoped QSettings .conf file — different
// vaults can have different look/behavior preferences. lastVaultPath stays
// app-scoped (small nested Settings below): it's last-used STATE needed to
// bootstrap which vault to open in the first place, not a vault-specific
// preference.
Item {
    id: appSettings

    // Set once by Main.qml (AppSettings.activeVaultModel = vaultModel) so
    // loadFromVault()/saveToVault() below don't need the caller to pass it
    // on every single settings change.
    property var activeVaultModel: null
    // True only while loadFromVault() is actively assigning properties —
    // suppresses the onXChanged-triggered save-back below, so loading never
    // immediately re-triggers a write of the same data it just read.
    property bool suppressSave: false

    Settings {
        id: appLevelSettings
        category: "app"
        property url lastVaultPath
    }
    property alias lastVaultPath: appLevelSettings.lastVaultPath

    property real borderRadius: 8
    property bool useSystemTitlebar: true

    // Shared accent color for hover outlines (HoverBorder.qml) and the
    // always-editable-field focus underline (UnderlineField.qml) — both
    // were hardcoded "white" until this was pulled out as one configurable
    // setting.
    property color accentColor: "white"

    property color sidebarBackgroundColor: "#1e1e1e"
    property real sidebarOpacity: 1.0
    property bool sidebarBlur: false

    property color mainBackgroundColor: "#252525"
    property real mainOpacity: 1.0
    property bool mainBlur: false

    // Global on/off for UI animations/transitions (recording feature adds
    // several new ones — e.g. level meters, recording-state pulses). Same
    // "opt-out feature flag" shape as sidebarBlur/mainBlur above.
    property bool animationsEnabled: true

    // While audio plays, the transcript auto-scrolls to keep the active
    // sentence pinned near the top of the view (MainPanel's
    // scrollActiveSentenceToTop). On by default — the toggle beside the play
    // button lets a user who prefers to scroll manually turn it off. Vault-
    // scoped like every other behavior toggle here.
    property bool followPlaybackScroll: true

    // Silence phase 2 (skip-silence during playback): while audio plays and
    // this is on, the head auto-seeks past any STORED `silences:` span (see
    // VadChunker::complementSpans / phase 1) whose length is >=
    // skipSilenceThresholdSeconds — see TranscriptionController::
    // nextSilenceSkipTarget and MainPanel.qml's player.positionChanged
    // Connections. Off by default (a behavior change to playback shouldn't
    // silently switch on). The threshold is a PURE UI FILTER over the
    // already-stored raw VAD spans — changing it never re-runs VAD or any
    // other task, only what counts as "long enough to skip" (locked Decision,
    // queue item 084). Vault-scoped like every other behavior toggle here.
    property bool skipSilenceEnabled: false
    property real skipSilenceThresholdSeconds: 3

    // AI auto-labeling (DESIGN.md "Model management"): opt-in second pass
    // after transcription, populates label/prelude/tags via LabelingEngine.
    // Off by default — it's an extra ~2.5GB model download plus GPU time
    // per file, not something to silently switch on for everyone.
    property bool autoLabelEnabled: false

    // Item 14: when autoLabelEnabled, tags freely generate new ones by
    // default (false). Restricting to the vault's existing tag vocabulary
    // is an explicit opt-in — auto-categorizing (leaving a file untagged
    // when nothing existing fits) is a deliberately different mode, not
    // the default behavior.
    property bool restrictTagsToExisting: false

    // Item 15 (Wave E): soft nudge (0.0-1.0) between inventing new tags vs.
    // reusing existing ones, when NOT hard-restricted (restrictTagsToExisting
    // above is the hard on/off switch; this is a gradient for when it's off).
    // Higher = more willing to reuse an existing tag even if it's only a
    // loose/"stretched" fit, rather than invent a precise new one.
    property real tagReuseBias: 0.5

    // Feature 1: how verbose the AI-generated label and prelude are, 0.0
    // (terse) to 1.0 (descriptive). Same vault-scoped/debounced-save/reset
    // mechanism as tagReuseBias above.
    property real wordiness: 0.5

    // Item 27: chapter-generation density (0.0-1.0) passed straight through
    // to TranscriptionController::generateChapters() (default 0.5). Unlike
    // wordiness/tagReuseBias (which map to 3 discrete prompt tiers) this is a
    // genuinely continuous knob — higher = more chapters. Same vault-scoped/
    // debounced-save/reset mechanism as the sliders above.
    property real chapterDensity: 0.5

    // Thin white translucent edge by default — "liquid glass" look.
    property bool rimEnabled: true
    property real rimWidth: 1
    property color rimColor: "#99ffffff"

    // "A recording is running" signal, shown app-wide so it survives
    // navigating away from the recorder page: the titlebar record button's
    // icon and the window rim both take this colour while
    // audioRecorder.recording is true. Defaults to the same soft red the
    // in-page stop button already uses (MainPanel's recordToggleButton), so
    // the three cues read as one state.
    property color recordingActiveColor: "#e07a7a"

    // false = "forgo labels": ignore (but don't delete) any stored label
    // frontmatter and rename the actual audio+transcript files instead.
    // Main-panel-only: it's the surface with rename UI (click-to-rename the
    // title) — Sidebar has no rename affordance, so its toggle (below) only
    // ever affects display, never rename behavior.
    property bool useLabelsMainPanel: true
    // Display-only (Sidebar rows have no rename UI): show each file's label
    // instead of its filename in the tree, when one exists.
    property bool useLabelsSidebar: true

    // Independently toggleable per surface (Milestone 5 item 20).
    property bool hideExtensionsSidebar: false
    property bool hideExtensionsMainPanel: false

    // Main panel's sentence-playback highlight (see MainPanel.qml's
    // sentenceHighlightOverlay) and Ctrl+F search-match highlight (Item 7,
    // Wave E: searchMatchOverlay) — both real overlay Rectangles. Color is
    // shared (highlightColor); radius has its OWN setting (highlightRadius),
    // separate from the generic UI borderRadius, since the user wants these
    // two independently adjustable.
    property color highlightColor: "#3a5a7a"
    property real highlightRadius: 8

    // Sidebar file-tree selected-row highlight (Sidebar.qml) — was
    // hardcoded translucent green (#2ecc7126 fill / #2ecc71 border); now a
    // configurable translucent-white fill + border, same shape as rimColor
    // above (color picker with alpha channel).
    property color sidebarSelectionColor: "#33ffffff"
    property color sidebarSelectionBorderColor: "#ffffff"

    // Sidebar file-tree row outlines (HoverBorder/selection highlight/
    // CombineGroupOutline) and their content (status dot/label) previously
    // hardcoded these two left-side gaps (user reports, 2026-07-12) — now
    // independently configurable, same shape as rimWidth/highlightRadius
    // above. sidebarEdgeLeftMargin is the outline's own inset from the
    // sidebar's true left edge; sidebarContentLeftMargin is the FURTHER gap
    // from the outline's left edge to where the row's own content (status
    // dot/label) starts. Left-only: top/right/bottom insets are unrelated
    // fixed values elsewhere (see Sidebar.qml's treeDelegate).
    property real sidebarEdgeLeftMargin: 4
    property real sidebarContentLeftMargin: 5

    // Sidebar file-tree rows' TOTAL right-side gap, from the sidebar's true
    // right edge to the outlines' (HoverBorder/selection highlight/
    // CombineGroupOutline) right edge — covers both the space the Basic
    // style's vertical ScrollBar overlay actually needs (~14px, see
    // Sidebar.qml's treeDelegate.implicitWidth doc comment) AND whatever
    // extra breathing room beyond that. Originally split across a fixed,
    // non-configurable -14 PLUS a separate configurable few-px extra, which
    // left a visible gap even with the extra dragged to 0 (user report,
    // 2026-07-12) — now ONE setting controls the whole thing, so 0 truly
    // means flush with the sidebar's right edge (and, deliberately, back
    // under the scrollbar overlay — the user's explicit choice at that
    // point). Default (17) reproduces the original fixed 14+3 look.
    property real sidebarRightMargin: 17

    // Transcript gutter's displayed timestamp precision — NOT the stored
    // data (segment lines always keep full sub-second precision on disk,
    // see TranscriptionController::formatTimestamp); this only rounds what
    // MainPanel's gutter shows. 0 (default) hides fractional seconds
    // entirely ("3", "1:07"); each increment adds one more decimal digit.
    property int timestampDecimalPlaces: 0

    // Default whisper model for new transcriptions — vault-scoped (a vault
    // of interview recordings may want a different default model than one
    // of podcast episodes). Matches ModelManager::kDefaultModelId.
    property string defaultModel: "base.en"

    // Speaker-diarization tuning — vault-scoped like defaultModel. Which
    // segmentation + embedding models to use (ids from
    // TranscriptionController::availableSegmentationModels()/...Embedding...).
    // diarizationSensitivity is the clustering distance threshold — a
    // continuous knob like chapterDensity: LOWER = stricter = MORE speakers,
    // HIGHER = more merging = FEWER speakers. It tunes the always-on
    // auto-detection; it is NOT a fixed speaker count. See DiarizationEngine.
    property string diarizationSegmentationModel: "pyannote-segmentation-3.0"
    property string diarizationEmbeddingModel: "wespeaker-resnet293-lm"
    property real diarizationSensitivity: 0.5

    // Sidebar file-tree order: "name" (default, alphabetical dirs-first —
    // matches this app's original/only behavior), "dateDesc" (Date Modified,
    // newest first), "dateAsc" (Date Modified, oldest first). Combine
    // candidates (see combineRules below) are always kept tree-adjacent
    // regardless of which of these is active — sortMode only decides the
    // order of groups and of files within a group, not whether they cluster.
    property string sortMode: "name"

    // BottomBar's opt-in alternative to the default ProgressBar: a thin
    // glowing line across the top edge of the bottom info bar. Off by
    // default (same "opt-in alternative style" reasoning as autoLabelEnabled
    // above) — same shape as the rimEnabled/rimWidth/rimColor triple, plus
    // one extra numeric (progressGlowRadius) for how far the glow blur
    // extends, since a glow needs both a thickness and a blur spread.
    property bool progressGlowEnabled: true
    property color progressGlowColor: "#ffffff"
    property real progressGlowWidth: 2
    property real progressGlowRadius: 8
    // How many stacked halo layers sit behind the opaque core line (the core
    // line itself is always present and NOT counted here). BottomBar.qml's
    // Repeater interpolates each layer's width and opacity across this count,
    // so 2 reproduces the original hand-written outer(~0.15)/inner(~0.35)
    // pair and higher counts smooth/deepen the glow.
    property int progressGlowLayerCount: 2
    // When the glow is enabled, hide the standard determinate ProgressBar so
    // the glow does the job alone. Independent of progressGlowEnabled so a
    // user can turn this off to show both at once.
    property bool progressGlowHidesBar: true

    // Bottom info bar's "Kind: filename" task label (see BottomBar.qml).
    // Clicking it opens that file in the main panel (same as a Tasks-popup row).
    property bool bottomBarTaskClickOpensFile: true
    // Hover tooltip showing the full, un-elided task text when it truncates.
    property bool bottomBarTaskTooltip: true
    // The bottom-left vault path sizes to its content instead of claiming the
    // whole row, freeing width for the task label so it truncates less.
    property bool bottomBarPathShrinkToContent: true
    // When the task label would still overflow, shrink its font (and wrap to a
    // second line) to fit instead of eliding.
    property bool bottomBarTaskTextFit: true

    // Split-recording combine (see TranscriptionController::
    // findCombineGroups()'s doc comment): what happens to a group's original
    // files once ffmpeg has merged them. false (default) moves them into a
    // sibling ".combined" folder instead of deleting — safer default until
    // the user's trusted their rules, matching this app's general "keep
    // until confident" bias around anything that touches source recordings.
    property bool deleteOriginalsAfterCombine: false

    // Run a combine scan automatically once a batch of USB auto-grabs has
    // finished (UsbGrabberController::autoGrabsSettled -> Main.qml). false
    // (default) keeps combine a purely explicit action.
    //
    // Narrow on purpose. Combine used to fire on every vault-tree rebuild, and
    // it was removed for good reason: it silently merged files the moment a
    // recorder dropped in a new part, surprising anyone watching a vault fill
    // up mid-recording (see Sidebar.qml's combineButton). This is not that
    // coming back. It's opt-in, and it fires on ONE discrete event — a device
    // the user chose to auto-grab has just finished offloading, which is
    // precisely when a split recording is complete and combining it is what
    // they'd do by hand anyway. It is not a general "combine whenever the vault
    // changes" mode, and it must never grow into one.
    //
    // Still rule-driven like every other combine: only groups a user-authored
    // combineRules entry matched, and only ones detectCombineGroups() reports
    // "ready", are ever enqueued. An empty combineRules means this does nothing.
    property bool autoCombineAfterGrab: false

    // Where new in-app recordings are written to. Empty string (default)
    // means "use vault root" — interpreted downstream by the recording
    // controller, not here.
    property string recordingOutputFolder: ""

    // Input gain multipliers (1.0 = unity) applied to the microphone and
    // system-audio capture streams before mixing, for the in-app recording
    // feature — independently adjustable since a user might want to boost a
    // quiet mic while leaving system audio untouched, or vice versa.
    property real recordingMicGain: 1.0
    property real recordingSystemGain: 1.0

    // Per-application capture state for the recorder's per-app rows, keyed by
    // the ancestry-resolved application display name ("Discord", not
    // "Chromium") — the same key the rows themselves use, and the only handle
    // stable enough to outlive both an app going quiet and this app
    // restarting. A "stream:<object.serial>" id is neither.
    //
    // A key is written ONLY when the user actually adjusts that app's gain or
    // toggles its row, never merely on seeing the app play. That keeps the map
    // to the handful of apps someone has deliberately set up, and makes its
    // key set double as "which rows to show before anything has played this
    // session".
    //
    // Deliberately NOT part of `defaults`/isDefault()/resetProperty() below,
    // for the same reason iconOverrides isn't (see its comment): that
    // machinery is single-value reset-to-default semantics compared with
    // `===`, which can't compare a map. Loaded/saved explicitly in
    // loadFromVault()/saveToVault() instead.
    property var recordingAppGains: ({})
    // Which of those apps are armed for capture. Separate from the gains map
    // rather than a nested {gain, enabled} object: the two are written from
    // different controls and JSON round-trips a flat map with no surprises.
    property var recordingAppCaptureEnabled: ({})

    // Which icon pack supplies icons by default — a pack id from
    // IconTheme::packIds() (packs are directories under assets/icons/, see
    // DESIGN.md "Iconography"). "material" (Google Material Symbols) is the
    // default; "classic" is the app's original hand-rolled glyph set, kept
    // as an opt-in pack. Per-icon exceptions go in iconOverrides below.
    property string iconPack: "material"

    // Per-icon pack exceptions: maps a semantic icon name to the pack id it
    // should come from, overriding iconPack for that one icon (e.g.
    // {"play": "classic"} to keep the hand-drawn triangle while everything
    // else stays Material). IconTheme::resolve() applies this first, then
    // iconPack, then its own fallback pack.
    //
    // Deliberately NOT part of `defaults`/isDefault()/resetProperty() below,
    // for the same reason combineRules isn't (see its comment): that
    // machinery is single-value reset-to-default semantics compared with
    // `===`, which can't compare a map. Empty ({}) is its own "default" and
    // the Settings UI manages entries directly. Loaded/saved explicitly in
    // loadFromVault()/saveToVault() instead.
    property var iconOverrides: ({})

    // Split-recording combine rules — a list of {name, pattern, enabled,
    // sortGroupIndex, sortGroupText, sortGroupOccurrence} maps, each a
    // QRegularExpression matched against a filename (see
    // findCombineGroups()'s doc comment for the capture-group-as-identity/
    // sort-differentiator semantics). sortGroupIndex/sortGroupText/
    // sortGroupOccurrence are optional and absent on any rule that hasn't
    // designated a sort differentiator — SettingsPopup.qml's
    // commitPatternChange()/resolveSortGroupIndex() are the only writers,
    // and always keep all three consistent with the current `pattern` (see
    // their doc comments for why sortGroupText+sortGroupOccurrence, not
    // just the numeric index, are the thing actually persisted as the
    // source of truth — occurrence specifically disambiguates two
    // differentiator runs that happen to share identical text). Deliberately
    // NOT part of `defaults`/isDefault()/
    // resetProperty() below: that machinery is single-value reset-to-default
    // semantics, which doesn't fit a user-authored list (no sensible
    // "default" rule set, and the Settings UI manages entries directly via
    // add/remove rather than a reset button) — see loadFromVault()/
    // saveToVault() for its own explicit (de)serialization instead.
    property var combineRules: []

    // Single source of truth for every per-setting/global "reset to
    // default" button, AND for loadFromVault()/saveToVault() below (every
    // key here round-trips through the vault's .transcriber/settings.json file).
    // lastVaultPath is intentionally excluded: it tracks last-used state,
    // not a vault-specific appearance/behavior setting.
    readonly property var defaults: ({
        borderRadius: 8,
        useSystemTitlebar: true,
        accentColor: "white",
        sidebarBackgroundColor: "#1e1e1e",
        sidebarOpacity: 1.0,
        sidebarBlur: false,
        mainBackgroundColor: "#252525",
        mainOpacity: 1.0,
        mainBlur: false,
        animationsEnabled: true,
        rimEnabled: true,
        rimWidth: 1,
        rimColor: "#99ffffff",
        recordingActiveColor: "#e07a7a",
        useLabelsMainPanel: true,
        useLabelsSidebar: true,
        hideExtensionsSidebar: false,
        hideExtensionsMainPanel: false,
        followPlaybackScroll: true,
        skipSilenceEnabled: false,
        skipSilenceThresholdSeconds: 3,
        autoLabelEnabled: false,
        restrictTagsToExisting: false,
        tagReuseBias: 0.5,
        wordiness: 0.5,
        chapterDensity: 0.5,
        diarizationSegmentationModel: "pyannote-segmentation-3.0",
        diarizationEmbeddingModel: "wespeaker-resnet293-lm",
        diarizationSensitivity: 0.5,
        highlightColor: "#3a5a7a",
        highlightRadius: 8,
        sidebarSelectionColor: "#33ffffff",
        sidebarSelectionBorderColor: "#ffffff",
        sidebarEdgeLeftMargin: 4,
        sidebarContentLeftMargin: 5,
        sidebarRightMargin: 17,
        timestampDecimalPlaces: 0,
        defaultModel: "base.en",
        sortMode: "name",
        iconPack: "material",
        progressGlowEnabled: true,
        progressGlowColor: "#ffffff",
        progressGlowWidth: 2,
        progressGlowRadius: 8,
        progressGlowLayerCount: 2,
        progressGlowHidesBar: true,
        bottomBarTaskClickOpensFile: true,
        bottomBarTaskTooltip: true,
        bottomBarPathShrinkToContent: true,
        bottomBarTaskTextFit: true,
        deleteOriginalsAfterCombine: false,
        autoCombineAfterGrab: false,
        recordingOutputFolder: "",
        recordingMicGain: 1.0,
        recordingSystemGain: 1.0
    })

    // QML `color` values need to be stringified before crossing into a
    // QVariantMap bound for JSON (QJsonValue::fromVariant doesn't know how
    // to serialize a QColor) — same detection idiom isDefault() already uses.
    function isColorValue(v) {
        return v instanceof Object && "r" in v && "g" in v && "b" in v
    }

    function isDefault(name) {
        if (!(name in defaults))
            return true
        const current = appSettings[name]
        const def = defaults[name]
        if (isColorValue(current))
            return Qt.colorEqual(current, def)
        return current === def
    }

    function resetProperty(name) {
        if (name in defaults)
            appSettings[name] = defaults[name]
    }

    function resetAll() {
        for (const key in defaults)
            resetProperty(key)
    }

    // Reads <vault>/.transcriber/settings.json (VaultTreeModel::readVaultSettings) and
    // applies every known key, falling back to its default if the file is
    // missing/empty (fresh vault) or doesn't have that particular key yet
    // (settings added in a later app version). suppressSave guards against
    // the property assignments below immediately triggering saveToVault().
    function loadFromVault(vaultModel) {
        appSettings.suppressSave = true
        appSettings.activeVaultModel = vaultModel
        const loaded = vaultModel ? vaultModel.readVaultSettings() : {}
        for (const key in appSettings.defaults)
            appSettings[key] = (key in loaded) ? loaded[key] : appSettings.defaults[key]
        // Neither combineRules nor iconOverrides is in `defaults` (see their
        // own doc comments above) — loaded explicitly here instead.
        appSettings.combineRules = loaded.combineRules || []
        appSettings.iconOverrides = loaded.iconOverrides || ({})
        appSettings.recordingAppGains = loaded.recordingAppGains || ({})
        appSettings.recordingAppCaptureEnabled = loaded.recordingAppCaptureEnabled || ({})
        appSettings.suppressSave = false
    }

    function saveToVault() {
        if (appSettings.suppressSave || !appSettings.activeVaultModel)
            return
        const data = ({})
        for (const key in appSettings.defaults) {
            const value = appSettings[key]
            data[key] = isColorValue(value) ? String(value) : value
        }
        data.combineRules = appSettings.combineRules
        data.iconOverrides = appSettings.iconOverrides
        data.recordingAppGains = appSettings.recordingAppGains
        data.recordingAppCaptureEnabled = appSettings.recordingAppCaptureEnabled
        appSettings.activeVaultModel.writeVaultSettings(data)
    }

    // Debounced so dragging a slider doesn't write the file on every pixel
    // of movement — coalesces into one write shortly after changes settle.
    Timer {
        id: saveTimer
        interval: 250
        onTriggered: appSettings.saveToVault()
    }
    function scheduleSave() {
        if (!appSettings.suppressSave)
            saveTimer.restart()
    }

    // Write any debounced-but-not-yet-flushed change RIGHT NOW. The app closes
    // via a hard ::_exit(0) (TranscriptionController::terminateApplicationImmediately)
    // which never lets saveTimer fire, so a setting changed within saveTimer's
    // interval of closing the window would otherwise be silently lost. Main.qml's
    // onClosing calls this before terminating.
    function flushPendingSave() {
        if (saveTimer.running) {
            saveTimer.stop()
            appSettings.saveToVault()
        }
    }

    // One handler per persisted key (mechanical, mirrors the defaults map
    // above) — QML has no generic "any property on this object changed"
    // signal, so each needs its own connection.
    Connections {
        target: appSettings
        function onBorderRadiusChanged() { appSettings.scheduleSave() }
        function onUseSystemTitlebarChanged() { appSettings.scheduleSave() }
        function onAccentColorChanged() { appSettings.scheduleSave() }
        function onSidebarBackgroundColorChanged() { appSettings.scheduleSave() }
        function onSidebarOpacityChanged() { appSettings.scheduleSave() }
        function onSidebarBlurChanged() { appSettings.scheduleSave() }
        function onMainBackgroundColorChanged() { appSettings.scheduleSave() }
        function onMainOpacityChanged() { appSettings.scheduleSave() }
        function onMainBlurChanged() { appSettings.scheduleSave() }
        function onAnimationsEnabledChanged() { appSettings.scheduleSave() }
        function onRimEnabledChanged() { appSettings.scheduleSave() }
        function onRimWidthChanged() { appSettings.scheduleSave() }
        function onRimColorChanged() { appSettings.scheduleSave() }
        function onRecordingActiveColorChanged() { appSettings.scheduleSave() }
        function onUseLabelsMainPanelChanged() { appSettings.scheduleSave() }
        function onUseLabelsSidebarChanged() { appSettings.scheduleSave() }
        function onHideExtensionsSidebarChanged() { appSettings.scheduleSave() }
        function onHideExtensionsMainPanelChanged() { appSettings.scheduleSave() }
        function onFollowPlaybackScrollChanged() { appSettings.scheduleSave() }
        function onSkipSilenceEnabledChanged() { appSettings.scheduleSave() }
        function onSkipSilenceThresholdSecondsChanged() { appSettings.scheduleSave() }
        function onAutoLabelEnabledChanged() { appSettings.scheduleSave() }
        function onRestrictTagsToExistingChanged() { appSettings.scheduleSave() }
        function onTagReuseBiasChanged() { appSettings.scheduleSave() }
        function onWordinessChanged() { appSettings.scheduleSave() }
        function onChapterDensityChanged() { appSettings.scheduleSave() }
        function onHighlightColorChanged() { appSettings.scheduleSave() }
        function onHighlightRadiusChanged() { appSettings.scheduleSave() }
        function onSidebarSelectionColorChanged() { appSettings.scheduleSave() }
        function onSidebarSelectionBorderColorChanged() { appSettings.scheduleSave() }
        function onSidebarEdgeLeftMarginChanged() { appSettings.scheduleSave() }
        function onSidebarContentLeftMarginChanged() { appSettings.scheduleSave() }
        function onSidebarRightMarginChanged() { appSettings.scheduleSave() }
        function onTimestampDecimalPlacesChanged() { appSettings.scheduleSave() }
        function onDefaultModelChanged() { appSettings.scheduleSave() }
        function onDiarizationSegmentationModelChanged() { appSettings.scheduleSave() }
        function onDiarizationEmbeddingModelChanged() { appSettings.scheduleSave() }
        function onDiarizationSensitivityChanged() { appSettings.scheduleSave() }
        function onSortModeChanged() { appSettings.scheduleSave() }
        function onIconPackChanged() { appSettings.scheduleSave() }
        function onProgressGlowEnabledChanged() { appSettings.scheduleSave() }
        function onProgressGlowColorChanged() { appSettings.scheduleSave() }
        function onProgressGlowWidthChanged() { appSettings.scheduleSave() }
        function onProgressGlowRadiusChanged() { appSettings.scheduleSave() }
        function onProgressGlowLayerCountChanged() { appSettings.scheduleSave() }
        function onProgressGlowHidesBarChanged() { appSettings.scheduleSave() }
        function onBottomBarTaskClickOpensFileChanged() { appSettings.scheduleSave() }
        function onBottomBarTaskTooltipChanged() { appSettings.scheduleSave() }
        function onBottomBarPathShrinkToContentChanged() { appSettings.scheduleSave() }
        function onBottomBarTaskTextFitChanged() { appSettings.scheduleSave() }
        function onDeleteOriginalsAfterCombineChanged() { appSettings.scheduleSave() }
        function onAutoCombineAfterGrabChanged() { appSettings.scheduleSave() }
        function onRecordingOutputFolderChanged() { appSettings.scheduleSave() }
        function onRecordingMicGainChanged() { appSettings.scheduleSave() }
        function onRecordingSystemGainChanged() { appSettings.scheduleSave() }
        // combineRules/iconOverrides aren't in `defaults` (see their own doc
        // comments) but still need the same debounced-save-on-change wiring
        // as every other persisted key.
        function onCombineRulesChanged() { appSettings.scheduleSave() }
        function onIconOverridesChanged() { appSettings.scheduleSave() }
        function onRecordingAppGainsChanged() { appSettings.scheduleSave() }
        function onRecordingAppCaptureEnabledChanged() { appSettings.scheduleSave() }
    }
}
