import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SuopTranscriber

// USB grabber top-level screen — extracted verbatim from MainPanel's inline
// usbGrabberPage Item (2026-07-19 decomposition). Self-contained: owns its
// device/rule/history snapshots and refresh/preview logic; the host only wires
// the controller + vault info and toggles `visible`.
Item {
    id: usbGrabberPage

    property UsbGrabberController usbGrabberController
    property string vaultLocalPath: ""
    property VaultTreeModel vaultTreeModel

    // Raw snapshots from UsbGrabberController — re-pulled by
    // refreshDevices()/refreshHistory() rather than bound live, since the
    // controller exposes plain Q_INVOKABLE getters (no per-device
    // changed signals to bind against). deviceRows below derives the
    // actual per-row display model from these two plus a fresh
    // ruleForDevice() read per device, so it stays current every time
    // either snapshot is refreshed.
    property var mountedDevices: []
    property var savedRules: []
    property var catchAllRule: ({enabled: false, extensionFilter: [], filenameRegex: "", preserveStructure: true, startFolder: "", deviceLabel: "", deviceSerial: "*", destinationFolder: ""})
    property var historyEntries: []

    function refreshDevices() {
        if (!usbGrabberPage.usbGrabberController) {
            usbGrabberPage.mountedDevices = []
            usbGrabberPage.savedRules = []
            usbGrabberPage.catchAllRule = ({enabled: false, extensionFilter: [], filenameRegex: "", preserveStructure: true, startFolder: "", deviceLabel: "", deviceSerial: "*", destinationFolder: ""})
            return
        }
        usbGrabberPage.mountedDevices = usbGrabberPage.usbGrabberController.mountedDevices()
        usbGrabberPage.savedRules = usbGrabberPage.usbGrabberController.allRules()
        usbGrabberPage.catchAllRule = usbGrabberPage.usbGrabberController.ruleForDevice("*")
    }

    // Newest first — the controller returns history in file-append
    // (oldest-first) order.
    function refreshHistory() {
        usbGrabberPage.historyEntries = usbGrabberPage.usbGrabberController
            ? usbGrabberPage.usbGrabberController.history().slice().reverse() : []
    }

    function formatTimestamp(iso) {
        const d = new Date(iso)
        return isNaN(d.getTime()) ? iso : Qt.formatDateTime(d, "yyyy-MM-dd hh:mm")
    }

    // Grab preview: which device's preview is currently shown (only one
    // at a time, rendered inline under that device's own card — see the
    // Repeater delegate below), its raw {path,status} entries once
    // requestGrabPreview()'s async scan completes, and a running/error
    // flag for the interim "Scanning…" state. Cleared/reset on every new
    // Preview click, not on every refreshDevices() poll tick, so an
    // in-flight or just-finished preview survives the 2.5s device poll.
    property string previewDeviceSerial: ""
    property var previewEntries: []
    property bool previewRunning: false
    property string previewError: ""

    // Destination-folder picker rejection (see usbDestinationFolderDialog
    // below) — StyleGuide.md's warning-banner convention (inline Label,
    // driven by a non-empty backing string, no dismiss button).
    property string destinationFolderError: ""

    function requestPreview(serial) {
        if (!usbGrabberPage.usbGrabberController)
            return
        usbGrabberPage.previewDeviceSerial = serial
        usbGrabberPage.previewEntries = []
        usbGrabberPage.previewError = ""
        usbGrabberPage.previewRunning = true
        usbGrabberPage.usbGrabberController.requestGrabPreview(serial)
    }

    // Put the preview panel away. Clearing previewDeviceSerial is all it
    // takes even mid-scan: onGrabPreviewReady discards any result whose
    // serial no longer matches, so an in-flight walk simply lands nowhere
    // (it copies nothing and writes nothing — there is no side effect to
    // undo, only a panel to close). Nothing else re-shows the panel; only
    // another Preview click does.
    function dismissPreview() {
        usbGrabberPage.previewDeviceSerial = ""
        usbGrabberPage.previewEntries = []
        usbGrabberPage.previewError = ""
        usbGrabberPage.previewRunning = false
    }

    // Same three-state palette across color/label/summary — "will grab"
    // reuses the same green the "connected" status label already uses
    // (see deviceCard's mounted Label below), so the preview's meaning
    // ("this device is eligible and this file will move") reads
    // consistently with the rest of the page.
    function previewStatusColor(status) {
        switch (status) {
        case "grab": return "#7ac07a"
        case "duplicate": return "#888888"
        case "filtered": return "#666666"
        default: return "#888888"
        }
    }

    function previewStatusLabel(status) {
        switch (status) {
        case "grab": return qsTr("will grab")
        case "duplicate": return qsTr("skip — already grabbed")
        case "filtered": return qsTr("skip — filtered out")
        default: return status
        }
    }

    function previewSummary(entries) {
        let grabCount = 0, dupCount = 0, filteredCount = 0
        for (const e of entries) {
            if (e.status === "grab") ++grabCount
            else if (e.status === "duplicate") ++dupCount
            else if (e.status === "filtered") ++filteredCount
        }
        return qsTr("%1 will be grabbed, %2 already grabbed, %3 filtered out")
                .arg(grabCount).arg(dupCount).arg(filteredCount)
    }

    function escapePreviewHtml(s) {
        return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    function previewHtml(entries) {
        return entries.map(function (e) {
            return "<span style=\"color:" + usbGrabberPage.previewStatusColor(e.status) + ";\">"
                    + usbGrabberPage.previewStatusLabel(e.status) + " — "
                    + usbGrabberPage.escapePreviewHtml(e.path) + "</span>"
        }).join("<br>")
    }

    // Resolves a FolderDialog's selectedFolder into a vault-relative
    // string suitable for a rule's destinationFolder field ("" = vault
    // root), or null if the chosen folder is outside the vault — same
    // shape as recordingFolderDialog's onAccepted below.
    function relativeToVault(localPath) {
        if (!usbGrabberPage.vaultTreeModel || !usbGrabberPage.vaultTreeModel.isPathInsideVault(localPath))
            return null
        const prefix = usbGrabberPage.vaultLocalPath + "/"
        return localPath === usbGrabberPage.vaultLocalPath ? ""
            : (localPath.startsWith(prefix) ? localPath.slice(prefix.length) : localPath)
    }

    // Shared by every per-field commit below: re-reads the device's
    // current rule (so fields this call doesn't touch are preserved),
    // applies one field, saves, then re-derives deviceRows via
    // refreshDevices() so the row reflects what was actually persisted.
    //
    // If this device's preview is currently on screen, it is now describing
    // a rule that no longer exists — re-run it so the filename regex (and
    // every other field: a destination change re-decides what counts as a
    // duplicate, an extension change re-decides what's filtered) is
    // previewed as edited, the way the combine rules' preview follows their
    // pattern field. Deliberately NOT per keystroke, unlike the validity
    // indicator beside the field: a preview is an async walk that stats
    // every file on a physical device (DESIGN.md "USB import"), so it's
    // driven by an actual commit and skipped entirely when the value didn't
    // change — onEditingFinished fires on every focus-out, edited or not.
    // requestPreview() re-reads the SAVED rule, so this must run after
    // saveRule() rather than against the in-hand value.
    function commitRuleField(serial, label, field, value) {
        if (!usbGrabberPage.usbGrabberController)
            return
        const rule = usbGrabberPage.usbGrabberController.ruleForDevice(serial)
        const changed = JSON.stringify(rule[field]) !== JSON.stringify(value)
        rule[field] = value
        if (label)
            rule.deviceLabel = label
        usbGrabberPage.usbGrabberController.saveRule(rule)
        usbGrabberPage.refreshDevices()
        // A "*" edit also re-runs it: an on-screen preview may be resolving
        // through the catch-all rule right now (a device whose own rule is
        // disabled falls back to it — see resolveEffectiveRule()), and from
        // here there's no way to tell whether it is, so re-run rather than
        // leave a stale preview standing. Re-previewing a device that
        // wasn't using the catch-all after all just reproduces what's
        // already shown.
        if (changed && usbGrabberPage.previewDeviceSerial !== ""
                && (usbGrabberPage.previewDeviceSerial === serial || serial === "*"))
            usbGrabberPage.requestPreview(usbGrabberPage.previewDeviceSerial)
    }

    // Union of currently-mounted devices and previously-configured ones
    // (from savedRules, excluding the "*" catch-all — that has its own
    // dedicated card below), GROUPED by serial, each row carrying a
    // fresh ruleForDevice() read so toggling/editing one field doesn't
    // require a full round-trip before the row reflects it.
    //
    // One row per physical device, not per volume: rules are keyed by
    // serial, so a device's volumes necessarily share one rule and showing
    // them as separate rows would render one rule as several independently-
    // toggleable-looking switches. mountedDevices is per-VOLUME though (see
    // UsbDeviceMonitor::currentlyMountedDevices()), so volumes sharing a
    // serial are collected into mountPaths rather than overwriting each
    // other — the row's volume count comes from that, and "Grab now" grabs
    // all of them via triggerManualGrab().
    property var deviceRows: {
        if (!usbGrabberPage.usbGrabberController)
            return []
        const bySerial = {}
        for (const d of usbGrabberPage.mountedDevices) {
            const existing = bySerial[d.serial]
            if (existing) {
                existing.mountPaths.push(d.mountPath)
                continue
            }
            const label = ((d.vendor || "") + " " + (d.model || "")).trim()
            bySerial[d.serial] = {serial: d.serial, label: label.length > 0 ? label : d.serial,
                                   mounted: true, mountPaths: [d.mountPath]}
        }
        for (const r of usbGrabberPage.savedRules) {
            if (r.deviceSerial === "*" || bySerial[r.deviceSerial])
                continue
            bySerial[r.deviceSerial] = {serial: r.deviceSerial, label: r.deviceLabel || r.deviceSerial,
                                         mounted: false, mountPaths: []}
        }
        const rows = []
        for (const serial in bySerial) {
            const row = bySerial[serial]
            const rule = usbGrabberPage.usbGrabberController.ruleForDevice(serial)
            row.enabled = rule.enabled
            row.autoGrab = rule.autoGrab
            row.extensionFilterText = (rule.extensionFilter || []).join(", ")
            row.filenameRegex = rule.filenameRegex
            row.preserveStructure = rule.preserveStructure
            row.startFolder = rule.startFolder
            row.destinationFolder = rule.destinationFolder
            rows.push(row)
        }
        return rows
    }

    onVisibleChanged: if (usbGrabberPage.visible) { usbGrabberPage.refreshDevices(); usbGrabberPage.refreshHistory() }
    Component.onCompleted: { usbGrabberPage.refreshDevices(); usbGrabberPage.refreshHistory() }

    // No live deviceAdded/deviceRemoved signal is exposed on
    // UsbGrabberController itself (only the underlying UsbDeviceMonitor
    // has those) — polling is simplest for a low-frequency, non-
    // performance-sensitive utility page like this one, and only runs
    // while the page is actually visible.
    Timer {
        interval: 2500
        running: usbGrabberPage.visible
        repeat: true
        onTriggered: usbGrabberPage.refreshDevices()
    }

    // A grab (manual or hotplug-triggered) just finished — refresh both
    // the history list and the device rows (a hotplug grab may have
    // introduced a device this page hasn't seen before).
    Connections {
        target: usbGrabberPage.usbGrabberController
        function onGrabFinished() { usbGrabberPage.refreshHistory(); usbGrabberPage.refreshDevices() }
        function onGrabPreviewReady(serial, entries, error) {
            if (serial !== usbGrabberPage.previewDeviceSerial)
                return
            usbGrabberPage.previewRunning = false
            usbGrabberPage.previewEntries = entries
            usbGrabberPage.previewError = error
        }
    }

    ScrollPage {
        id: usbScrollArea
        anchors.fill: parent

        ColumnLayout {
            width: usbScrollArea.availableContentWidth
            spacing: 16

            Label {
                text: qsTr("USB Grabber")
                color: "white"
                font.bold: true
                font.pixelSize: 16
            }

            Label {
                Layout.fillWidth: true
                visible: usbGrabberPage.usbGrabberController && usbGrabberPage.usbGrabberController.lastError !== ""
                text: usbGrabberPage.usbGrabberController ? usbGrabberPage.usbGrabberController.lastError : ""
                color: "#e07a7a"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: usbGrabberPage.destinationFolderError !== ""
                text: usbGrabberPage.destinationFolderError
                color: "#e07a7a"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            // "Any device" catch-all rule card — same shape as a
            // per-device card below, minus the mounted/grab-now concept
            // (it isn't tied to one physical device).
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#3a3a3a"
                }

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Any USB device")
                        color: "white"
                    }
                    // Auto-grab is strictly narrower than enabled (see
                    // UsbGrabRule::autoGrab) — meaningless on a device that
                    // can't be grabbed at all, so it's disabled until the
                    // rule is on rather than sitting there as a live switch
                    // that quietly does nothing.
                    UsbToggle {
                        text: qsTr("Auto-grab")
                        enabled: usbGrabberPage.catchAllRule.enabled
                        checked: usbGrabberPage.catchAllRule.autoGrab || false
                        tooltip: qsTr("Grab from any matching device as soon as it's plugged in, without asking")
                        onToggled: (v) => usbGrabberPage.commitRuleField("*", "", "autoGrab", v)
                    }
                    UsbToggle {
                        text: qsTr("Enabled")
                        checked: usbGrabberPage.catchAllRule.enabled
                        onToggled: (v) => usbGrabberPage.commitRuleField("*", "", "enabled", v)
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: catchAllExtensionField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Extensions (comma-separated, blank = all files)")
                        text: (usbGrabberPage.catchAllRule.extensionFilter || []).join(", ")
                        background: UnderlineField { focused: catchAllExtensionField.activeFocus }
                        onEditingFinished: usbGrabberPage.commitRuleField("*", "", "extensionFilter",
                            text.split(",").map(s => s.trim()).filter(s => s.length > 0))
                        // Item 13: editable text needs an I-beam.
                        HoverHandler { cursorShape: Qt.IBeamCursor }
                    }
                    Switch {
                        id: catchAllPreserveSwitch
                        text: qsTr("Preserve folders")
                        checked: usbGrabberPage.catchAllRule.preserveStructure
                        hoverEnabled: true
                        onToggled: usbGrabberPage.commitRuleField("*", "", "preserveStructure", checked)
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        HoverBorder { anchors.fill: parent; active: catchAllPreserveSwitch.hovered }
                    }
                }

                UsbRegexField {
                    Layout.fillWidth: true
                    pattern: usbGrabberPage.catchAllRule.filenameRegex || ""
                    controller: usbGrabberPage.usbGrabberController
                    onCommitted: (newPattern) => usbGrabberPage.commitRuleField("*", "", "filenameRegex", newPattern)
                }

                TextField {
                    id: catchAllStartFolderField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Start folder (relative to device root, blank = root)")
                    text: usbGrabberPage.catchAllRule.startFolder || ""
                    background: UnderlineField { focused: catchAllStartFolderField.activeFocus }
                    onEditingFinished: usbGrabberPage.commitRuleField("*", "", "startFolder", text)
                    // Item 13: editable text needs an I-beam.
                    HoverHandler { cursorShape: Qt.IBeamCursor }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Destination: %1").arg(
                            usbGrabberPage.catchAllRule.destinationFolder || qsTr("vault root"))
                        color: "#888888"
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                    }
                    Item {
                        id: catchAllDestinationButton
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        HoverHandler { id: catchAllDestinationHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: {
                                const sub = usbGrabberPage.catchAllRule.destinationFolder
                                usbDestinationFolderDialog.targetSerial = "*"
                                usbDestinationFolderDialog.targetLabel = ""
                                usbDestinationFolderDialog.currentFolder =
                                    "file://" + usbGrabberPage.vaultLocalPath + (sub ? "/" + sub : "")
                                usbDestinationFolderDialog.open()
                            }
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: AppSettings.borderRadius
                            color: catchAllDestinationHover.hovered ? "#3a3a3a" : "#2a2a2a"
                        }
                        HoverBorder { anchors.fill: parent; active: catchAllDestinationHover.hovered }
                        ToolTip.visible: catchAllDestinationHover.hovered
                        ToolTip.text: qsTr("Choose destination folder")
                        ToolTip.delay: 500
                        AppIcon { anchors.centerIn: parent; name: "folder"; iconSize: 13; color: "#dddddd" }
                    }
                }
            }

            Label {
                text: qsTr("Devices")
                color: "#cccccc"
                font.pixelSize: 13
            }

            Label {
                Layout.fillWidth: true
                visible: usbGrabberPage.deviceRows.length === 0
                text: qsTr("No USB devices seen yet — plug one in, or rely on \"Any USB device\" above.")
                color: "#888888"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: usbGrabberPage.deviceRows

                delegate: ColumnLayout {
                    id: deviceCard
                    required property var modelData

                    Layout.fillWidth: true
                    spacing: 4

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: "#3a3a3a"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: deviceCard.modelData.label
                            color: "white"
                            elide: Text.ElideRight
                        }
                        Label {
                            // A multi-LUN device (e.g. a recorder exposing internal
                            // memory and an SD card) mounts several volumes under one
                            // serial and one rule — say so, since "connected" alone
                            // would hide that "Grab now" is about to walk more than
                            // one of them.
                            text: !deviceCard.modelData.mounted ? qsTr("not connected")
                                : deviceCard.modelData.mountPaths.length > 1
                                    ? qsTr("%1 volumes connected").arg(deviceCard.modelData.mountPaths.length)
                                    : qsTr("connected")
                            color: deviceCard.modelData.mounted ? "#7ac07a" : "#888888"
                            font.pixelSize: 11
                        }
                        // Strictly narrower than the enable switch beside it
                        // (see UsbGrabRule::autoGrab) — meaningless on a
                        // device that can't be grabbed at all, so it's
                        // disabled until the rule is on rather than sitting
                        // there as a live switch that quietly does nothing.
                        UsbToggle {
                            text: qsTr("Auto-grab")
                            enabled: deviceCard.modelData.enabled
                            checked: deviceCard.modelData.autoGrab || false
                            tooltip: qsTr("Grab from this device as soon as it's plugged in, without asking")
                            onToggled: (v) => usbGrabberPage.commitRuleField(deviceCard.modelData.serial,
                                deviceCard.modelData.label, "autoGrab", v)
                        }
                        UsbToggle {
                            text: qsTr("Enabled")
                            checked: deviceCard.modelData.enabled
                            onToggled: (v) => usbGrabberPage.commitRuleField(deviceCard.modelData.serial,
                                deviceCard.modelData.label, "enabled", v)
                        }
                        Button {
                            id: grabNowButton
                            text: qsTr("Grab now")
                            implicitHeight: 22
                            enabled: deviceCard.modelData.mounted
                            opacity: enabled ? 1 : 0.35
                            hoverEnabled: enabled
                            onClicked: usbGrabberPage.usbGrabberController.triggerManualGrab(deviceCard.modelData.serial)
                            background: Rectangle {
                                radius: AppSettings.borderRadius
                                color: "transparent"
                                border.width: 1
                                border.color: "#3a3a3a"
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: grabNowButton.enabled }
                            HoverBorder { anchors.fill: parent; active: grabNowButton.hovered }
                        }
                        Button {
                            id: previewGrabButton
                            text: qsTr("Preview")
                            implicitHeight: 22
                            enabled: deviceCard.modelData.mounted
                            opacity: enabled ? 1 : 0.35
                            hoverEnabled: enabled
                            onClicked: usbGrabberPage.requestPreview(deviceCard.modelData.serial)
                            background: Rectangle {
                                radius: AppSettings.borderRadius
                                color: "transparent"
                                border.width: 1
                                border.color: "#3a3a3a"
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: previewGrabButton.enabled }
                            HoverBorder { anchors.fill: parent; active: previewGrabButton.hovered }
                        }
                        Button {
                            id: forgetDeviceButton
                            text: qsTr("Forget")
                            implicitHeight: 22
                            hoverEnabled: true
                            onClicked: {
                                usbGrabberPage.usbGrabberController.removeRule(deviceCard.modelData.serial)
                                usbGrabberPage.refreshDevices()
                            }
                            background: Rectangle {
                                radius: AppSettings.borderRadius
                                color: "transparent"
                                border.width: 1
                                border.color: "#3a3a3a"
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            HoverBorder { anchors.fill: parent; active: forgetDeviceButton.hovered }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: deviceExtensionField
                            Layout.fillWidth: true
                            placeholderText: qsTr("Extensions (comma-separated, blank = all files)")
                            text: deviceCard.modelData.extensionFilterText
                            background: UnderlineField { focused: deviceExtensionField.activeFocus }
                            onEditingFinished: usbGrabberPage.commitRuleField(deviceCard.modelData.serial,
                                deviceCard.modelData.label, "extensionFilter",
                                text.split(",").map(s => s.trim()).filter(s => s.length > 0))
                            // Item 13: editable text needs an I-beam.
                            HoverHandler { cursorShape: Qt.IBeamCursor }
                        }
                        Switch {
                            id: devicePreserveSwitch
                            text: qsTr("Preserve folders")
                            checked: deviceCard.modelData.preserveStructure
                            hoverEnabled: true
                            onToggled: usbGrabberPage.commitRuleField(deviceCard.modelData.serial,
                                deviceCard.modelData.label, "preserveStructure", checked)
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            HoverBorder { anchors.fill: parent; active: devicePreserveSwitch.hovered }
                        }
                    }

                    UsbRegexField {
                        Layout.fillWidth: true
                        pattern: deviceCard.modelData.filenameRegex || ""
                        controller: usbGrabberPage.usbGrabberController
                        onCommitted: (newPattern) => usbGrabberPage.commitRuleField(
                            deviceCard.modelData.serial, deviceCard.modelData.label,
                            "filenameRegex", newPattern)
                    }

                    TextField {
                        id: deviceStartFolderField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Start folder (relative to device root, blank = root)")
                        text: deviceCard.modelData.startFolder || ""
                        background: UnderlineField { focused: deviceStartFolderField.activeFocus }
                        onEditingFinished: usbGrabberPage.commitRuleField(deviceCard.modelData.serial,
                            deviceCard.modelData.label, "startFolder", text)
                        // Item 13: editable text needs an I-beam.
                        HoverHandler { cursorShape: Qt.IBeamCursor }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Destination: %1").arg(
                                deviceCard.modelData.destinationFolder || qsTr("vault root"))
                            color: "#888888"
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                        }
                        Item {
                            id: deviceDestinationButton
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                            HoverHandler { id: deviceDestinationHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                onTapped: {
                                    const sub = deviceCard.modelData.destinationFolder
                                    usbDestinationFolderDialog.targetSerial = deviceCard.modelData.serial
                                    usbDestinationFolderDialog.targetLabel = deviceCard.modelData.label
                                    usbDestinationFolderDialog.currentFolder =
                                        "file://" + usbGrabberPage.vaultLocalPath + (sub ? "/" + sub : "")
                                    usbDestinationFolderDialog.open()
                                }
                            }
                            Rectangle {
                                anchors.fill: parent
                                radius: AppSettings.borderRadius
                                color: deviceDestinationHover.hovered ? "#3a3a3a" : "#2a2a2a"
                            }
                            HoverBorder { anchors.fill: parent; active: deviceDestinationHover.hovered }
                            ToolTip.visible: deviceDestinationHover.hovered
                            ToolTip.text: qsTr("Choose destination folder")
                            ToolTip.delay: 500
                            AppIcon { anchors.centerIn: parent; name: "folder"; iconSize: 13; color: "#dddddd" }
                        }
                    }

                    // Grab preview — rendered right under whichever device's
                    // "Preview" button was last clicked (see
                    // usbGrabberPage.requestPreview()/previewDeviceSerial),
                    // styled like SettingsPopup.qml's combine-rule preview
                    // cards (dark card, rich-text lines, per-status color).
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        visible: deviceCard.modelData.serial === usbGrabberPage.previewDeviceSerial

                        // Preview header. The dismiss control lives HERE
                        // rather than inside the results card below so it's
                        // reachable in every preview state — scanning,
                        // refused, empty, and results — not only the one
                        // that happened to find files. Nothing else ever
                        // takes the panel down (deliberately: it survives
                        // the 2.5s device poll, see previewDeviceSerial),
                        // so without this the only way out was previewing a
                        // different device or leaving the page.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Preview")
                                color: "#888888"
                                font.pixelSize: 11
                            }
                            Item {
                                id: dismissPreviewButton
                                Layout.preferredWidth: 20
                                Layout.preferredHeight: 20

                                HoverHandler {
                                    id: dismissPreviewHover
                                    cursorShape: Qt.PointingHandCursor
                                }
                                TapHandler { onTapped: usbGrabberPage.dismissPreview() }
                                HoverBorder { anchors.fill: parent; active: dismissPreviewHover.hovered }
                                AppIcon {
                                    anchors.centerIn: parent
                                    name: "close"
                                    iconSize: 13
                                    color: "#dddddd"
                                }
                                ToolTip.visible: dismissPreviewHover.hovered
                                ToolTip.delay: 500
                                ToolTip.text: qsTr("Dismiss preview")
                            }
                        }

                        Label {
                            visible: usbGrabberPage.previewRunning
                            text: qsTr("Scanning…")
                            color: "#888888"
                            font.pixelSize: 11
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: !usbGrabberPage.previewRunning && usbGrabberPage.previewError !== ""
                            text: usbGrabberPage.previewError
                            color: "#e07a7a"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                        Label {
                            visible: !usbGrabberPage.previewRunning && usbGrabberPage.previewError === ""
                                    && usbGrabberPage.previewEntries.length === 0
                            text: qsTr("No files found.")
                            color: "#888888"
                            font.pixelSize: 11
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: previewCardColumn.implicitHeight + 12
                            visible: !usbGrabberPage.previewRunning && usbGrabberPage.previewEntries.length > 0
                            radius: AppSettings.borderRadius
                            color: "#1e1e1e"
                            border.width: 1
                            border.color: "#3a3a3a"

                            ColumnLayout {
                                id: previewCardColumn
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 2

                                Label {
                                    Layout.fillWidth: true
                                    text: usbGrabberPage.previewSummary(usbGrabberPage.previewEntries)
                                    color: "#dddddd"
                                    font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                }
                                TextEdit {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    readOnly: true
                                    selectByMouse: true
                                    selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)
                                    color: "#dddddd"
                                    font.pixelSize: 10
                                    textFormat: Text.RichText
                                    text: usbGrabberPage.previewHtml(usbGrabberPage.previewEntries)
                                    // Item 13: selectable text needs an I-beam.
                                    HoverHandler { cursorShape: Qt.IBeamCursor }
                                }
                            }
                        }
                    }
                }
            }

            Label {
                text: qsTr("History")
                color: "#cccccc"
                font.pixelSize: 13
            }

            Label {
                Layout.fillWidth: true
                visible: usbGrabberPage.historyEntries.length === 0
                text: qsTr("No grabs yet.")
                color: "#888888"
                font.pixelSize: 11
            }

            Repeater {
                model: usbGrabberPage.historyEntries

                delegate: ColumnLayout {
                    id: historyCard
                    required property var modelData

                    Layout.fillWidth: true
                    spacing: 2

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: "#2a2a2a"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: usbGrabberPage.formatTimestamp(historyCard.modelData.timestamp)
                            color: "#888888"
                            font.pixelSize: 11
                        }
                        Label {
                            Layout.fillWidth: true
                            text: historyCard.modelData.deviceLabel || historyCard.modelData.deviceSerial
                            color: "white"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        Label {
                            text: qsTr("%1 copied, %2 skipped").arg(historyCard.modelData.filesCopied)
                                .arg(historyCard.modelData.filesSkipped)
                            color: "#cccccc"
                            font.pixelSize: 11
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: (historyCard.modelData.errors || []).length > 0
                        text: (historyCard.modelData.errors || []).join("; ")
                        color: "#e07a7a"
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    // Shared by both the catch-all card and every per-device card's
    // destination-folder button above — targetSerial/targetLabel record
    // which rule to commit into on accept (mirrors recordingFolderDialog's
    // single-shared-dialog shape further below).
    FolderDialog {
        id: usbDestinationFolderDialog
        property string targetSerial: ""
        property string targetLabel: ""
        onAccepted: {
            const localPath = usbDestinationFolderDialog.selectedFolder.toString().replace("file://", "")
            const relative = usbGrabberPage.relativeToVault(localPath)
            if (relative === null) {
                usbGrabberPage.destinationFolderError =
                    qsTr("That folder is outside the vault — pick a location inside it.")
                return
            }
            usbGrabberPage.destinationFolderError = ""
            usbGrabberPage.commitRuleField(usbDestinationFolderDialog.targetSerial,
                usbDestinationFolderDialog.targetLabel, "destinationFolder", relative)
        }
    }
}
