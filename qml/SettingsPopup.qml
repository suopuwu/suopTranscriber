import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SuopTranscriber

Popup {
    id: settingsPopup

    signal changeVaultRequested()

    width: 440
    height: Math.min(contentColumn.implicitHeight + 32, maxHeight)
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
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property real maxHeight: parent ? parent.height * 0.9 : 600

    // While any slider in here is actively being dragged, hide this popup's
    // modal dimmer and its own visual content so the user can see the live
    // effect on the app behind it — reappears the instant the drag ends.
    // The Popup itself stays open/modal (visible/opacity unchanged) so the
    // slider's mouse grab isn't interrupted mid-drag.
    property bool anyPressed: radiusSlider.pressed || sidebarOpacitySlider.pressed
                               || mainOpacitySlider.pressed || rimWidthSlider.pressed
                               || highlightRadiusSlider.pressed || tagReuseBiasSlider.pressed
                               || timestampDecimalPlacesSlider.pressed
                               || progressGlowWidthSlider.pressed || progressGlowRadiusSlider.pressed

    dim: !anyPressed

    // Covers every key in AppSettings.defaults; used to enable/disable the
    // global "Reset to defaults" button. Reactive because it only reads
    // AppSettings properties (via isDefault), no manual signal wiring needed.
    function anySettingDirty() {
        return !AppSettings.isDefault("borderRadius")
                || !AppSettings.isDefault("useSystemTitlebar")
                || !AppSettings.isDefault("sidebarBackgroundColor")
                || !AppSettings.isDefault("sidebarOpacity")
                || !AppSettings.isDefault("sidebarBlur")
                || !AppSettings.isDefault("mainBackgroundColor")
                || !AppSettings.isDefault("mainOpacity")
                || !AppSettings.isDefault("mainBlur")
                || !AppSettings.isDefault("rimEnabled")
                || !AppSettings.isDefault("rimWidth")
                || !AppSettings.isDefault("rimColor")
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
                || !AppSettings.isDefault("timestampDecimalPlaces")
                || !AppSettings.isDefault("progressGlowEnabled")
                || !AppSettings.isDefault("progressGlowColor")
                || !AppSettings.isDefault("progressGlowWidth")
                || !AppSettings.isDefault("progressGlowRadius")
    }

    background: Rectangle {
        color: "#242424"
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: "#3a3a3a"
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

    ScrollView {
        id: scrollArea
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.topMargin: 16
        anchors.bottomMargin: 16
        // Reduced by the same 14px contentColumn reserves below for the
        // scrollbar overlay — keeps the CONTENT symmetric (16px inset on
        // both sides) by giving the scrollbar its own lane inside the right
        // margin, instead of 14px of clearance stacking on top of a full
        // 16px margin and making the popup look lopsided.
        anchors.rightMargin: 2
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        opacity: settingsPopup.anyPressed ? 0 : 1

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

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Settings")
                    color: "white"
                    font.bold: true
                    font.pixelSize: 16
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
                        color: (resetAllButton.hovered && resetAllButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
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

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Vault")
                }
                Button {
                    id: changeVaultButton
                    text: qsTr("Change Vault")
                    hoverEnabled: true
                    onClicked: settingsPopup.changeVaultRequested()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (changeVaultButton.hovered && changeVaultButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: changeVaultButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("Appearance")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Border radius")
                }
                Slider {
                    id: radiusSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 24
                    value: AppSettings.borderRadius
                    hoverEnabled: true
                    onMoved: AppSettings.borderRadius = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: radiusSlider.hovered
                    }
                }
                TextField {
                    id: radiusField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: radiusSlider.from; top: radiusSlider.to; decimals: 1 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.borderRadius = Math.max(radiusSlider.from, Math.min(radiusSlider.to, v))
                        text = AppSettings.borderRadius.toFixed(1)
                    }

                    background: UnderlineField { focused: radiusField.activeFocus }

                    Binding {
                        target: radiusField
                        property: "text"
                        value: AppSettings.borderRadius.toFixed(1)
                        when: !radiusField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: radiusField.hovered
                    }
                }
                Button {
                    id: radiusResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("borderRadius")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("borderRadius")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: radiusResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: radiusResetButton.enabled && radiusResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Use system titlebar")
                }
                Switch {
                    id: titlebarSwitch
                    checked: AppSettings.useSystemTitlebar
                    hoverEnabled: true
                    onToggled: AppSettings.useSystemTitlebar = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: titlebarSwitch.hovered
                    }
                }
                Button {
                    id: titlebarResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("useSystemTitlebar")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("useSystemTitlebar")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: titlebarResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: titlebarResetButton.enabled && titlebarResetButton.hovered
                    }
                }
            }

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
                        color: (sidebarColorButton.hovered && sidebarColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarColorButton.hovered
                    }
                }
                Button {
                    id: sidebarColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("sidebarBackgroundColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("sidebarBackgroundColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: sidebarColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarColorResetButton.enabled && sidebarColorResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Opacity")
                }
                Slider {
                    id: sidebarOpacitySlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 1
                    value: AppSettings.sidebarOpacity
                    hoverEnabled: true
                    onMoved: AppSettings.sidebarOpacity = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarOpacitySlider.hovered
                    }
                }
                TextField {
                    id: sidebarOpacityField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: sidebarOpacitySlider.from; top: sidebarOpacitySlider.to; decimals: 2 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.sidebarOpacity = Math.max(sidebarOpacitySlider.from, Math.min(sidebarOpacitySlider.to, v))
                        text = AppSettings.sidebarOpacity.toFixed(2)
                    }

                    background: UnderlineField { focused: sidebarOpacityField.activeFocus }

                    Binding {
                        target: sidebarOpacityField
                        property: "text"
                        value: AppSettings.sidebarOpacity.toFixed(2)
                        when: !sidebarOpacityField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarOpacityField.hovered
                    }
                }
                Button {
                    id: sidebarOpacityResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("sidebarOpacity")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("sidebarOpacity")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: sidebarOpacityResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarOpacityResetButton.enabled && sidebarOpacityResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Blur")
                }
                Switch {
                    id: sidebarBlurSwitch
                    checked: AppSettings.sidebarBlur
                    hoverEnabled: true
                    onToggled: AppSettings.sidebarBlur = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarBlurSwitch.hovered
                    }
                }
                Button {
                    id: sidebarBlurResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("sidebarBlur")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("sidebarBlur")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: sidebarBlurResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarBlurResetButton.enabled && sidebarBlurResetButton.hovered
                    }
                }
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
                        color: (sidebarSelectionColorButton.hovered && sidebarSelectionColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarSelectionColorButton.hovered
                    }
                }
                Button {
                    id: sidebarSelectionColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("sidebarSelectionColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("sidebarSelectionColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: sidebarSelectionColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarSelectionColorResetButton.enabled && sidebarSelectionColorResetButton.hovered
                    }
                }
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
                        color: (sidebarSelectionBorderColorButton.hovered && sidebarSelectionBorderColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarSelectionBorderColorButton.hovered
                    }
                }
                Button {
                    id: sidebarSelectionBorderColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("sidebarSelectionBorderColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("sidebarSelectionBorderColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: sidebarSelectionBorderColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarSelectionBorderColorResetButton.enabled && sidebarSelectionBorderColorResetButton.hovered
                    }
                }
            }

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
                        color: (mainColorButton.hovered && mainColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainColorButton.hovered
                    }
                }
                Button {
                    id: mainColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("mainBackgroundColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("mainBackgroundColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: mainColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainColorResetButton.enabled && mainColorResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Opacity")
                }
                Slider {
                    id: mainOpacitySlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 1
                    value: AppSettings.mainOpacity
                    hoverEnabled: true
                    onMoved: AppSettings.mainOpacity = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainOpacitySlider.hovered
                    }
                }
                TextField {
                    id: mainOpacityField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: mainOpacitySlider.from; top: mainOpacitySlider.to; decimals: 2 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.mainOpacity = Math.max(mainOpacitySlider.from, Math.min(mainOpacitySlider.to, v))
                        text = AppSettings.mainOpacity.toFixed(2)
                    }

                    background: UnderlineField { focused: mainOpacityField.activeFocus }

                    Binding {
                        target: mainOpacityField
                        property: "text"
                        value: AppSettings.mainOpacity.toFixed(2)
                        when: !mainOpacityField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainOpacityField.hovered
                    }
                }
                Button {
                    id: mainOpacityResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("mainOpacity")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("mainOpacity")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: mainOpacityResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainOpacityResetButton.enabled && mainOpacityResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Blur")
                }
                Switch {
                    id: mainBlurSwitch
                    checked: AppSettings.mainBlur
                    hoverEnabled: true
                    onToggled: AppSettings.mainBlur = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainBlurSwitch.hovered
                    }
                }
                Button {
                    id: mainBlurResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("mainBlur")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("mainBlur")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: mainBlurResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainBlurResetButton.enabled && mainBlurResetButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("Rim")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Enabled")
                }
                Switch {
                    id: rimEnabledSwitch
                    checked: AppSettings.rimEnabled
                    hoverEnabled: true
                    onToggled: AppSettings.rimEnabled = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimEnabledSwitch.hovered
                    }
                }
                Button {
                    id: rimEnabledResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("rimEnabled")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("rimEnabled")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: rimEnabledResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimEnabledResetButton.enabled && rimEnabledResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Width")
                }
                Slider {
                    id: rimWidthSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 8
                    value: AppSettings.rimWidth
                    hoverEnabled: true
                    onMoved: AppSettings.rimWidth = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimWidthSlider.hovered
                    }
                }
                TextField {
                    id: rimWidthField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: rimWidthSlider.from; top: rimWidthSlider.to; decimals: 1 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.rimWidth = Math.max(rimWidthSlider.from, Math.min(rimWidthSlider.to, v))
                        text = AppSettings.rimWidth.toFixed(1)
                    }

                    background: UnderlineField { focused: rimWidthField.activeFocus }

                    Binding {
                        target: rimWidthField
                        property: "text"
                        value: AppSettings.rimWidth.toFixed(1)
                        when: !rimWidthField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimWidthField.hovered
                    }
                }
                Button {
                    id: rimWidthResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("rimWidth")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("rimWidth")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: rimWidthResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimWidthResetButton.enabled && rimWidthResetButton.hovered
                    }
                }
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
                        color: (rimColorButton.hovered && rimColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimColorButton.hovered
                    }
                }
                Button {
                    id: rimColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("rimColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("rimColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: rimColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimColorResetButton.enabled && rimColorResetButton.hovered
                    }
                }
            }

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
                        color: (highlightColorButton.hovered && highlightColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: highlightColorButton.hovered
                    }
                }
                Button {
                    id: highlightColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("highlightColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("highlightColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: highlightColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: highlightColorResetButton.enabled && highlightColorResetButton.hovered
                    }
                }
            }

            // Item 7 (Wave E): playback + Ctrl+F search-match highlight radius,
            // now independently adjustable from the app-wide border radius
            // above (was previously hardcoded to follow it).
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Radius")
                }
                Slider {
                    id: highlightRadiusSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 24
                    value: AppSettings.highlightRadius
                    hoverEnabled: true
                    onMoved: AppSettings.highlightRadius = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: highlightRadiusSlider.hovered
                    }
                }
                TextField {
                    id: highlightRadiusField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: highlightRadiusSlider.from; top: highlightRadiusSlider.to; decimals: 1 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.highlightRadius = Math.max(highlightRadiusSlider.from, Math.min(highlightRadiusSlider.to, v))
                        text = AppSettings.highlightRadius.toFixed(1)
                    }

                    background: UnderlineField { focused: highlightRadiusField.activeFocus }

                    Binding {
                        target: highlightRadiusField
                        property: "text"
                        value: AppSettings.highlightRadius.toFixed(1)
                        when: !highlightRadiusField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: highlightRadiusField.hovered
                    }
                }
                Button {
                    id: highlightRadiusResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("highlightRadius")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("highlightRadius")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: highlightRadiusResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: highlightRadiusResetButton.enabled && highlightRadiusResetButton.hovered
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Applies to the playback sentence highlight and the Ctrl+F search-match highlight.")
                color: "#888888"
                font.pixelSize: 11
            }

            Label {
                text: qsTr("Progress Glow")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Enabled")
                }
                Switch {
                    id: progressGlowEnabledSwitch
                    checked: AppSettings.progressGlowEnabled
                    hoverEnabled: true
                    onToggled: AppSettings.progressGlowEnabled = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowEnabledSwitch.hovered
                    }
                }
                Button {
                    id: progressGlowEnabledResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("progressGlowEnabled")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("progressGlowEnabled")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: progressGlowEnabledResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowEnabledResetButton.enabled && progressGlowEnabledResetButton.hovered
                    }
                }
            }

            Label {
                visible: progressGlowEnabledSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Replaces the bottom info bar's progress bar with a thin glowing line across its top edge.")
                color: "#888888"
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
                        color: (progressGlowColorButton.hovered && progressGlowColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowColorButton.hovered
                    }
                }
                Button {
                    id: progressGlowColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("progressGlowColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("progressGlowColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: progressGlowColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowColorResetButton.enabled && progressGlowColorResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: progressGlowEnabledSwitch.checked
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Width")
                }
                Slider {
                    id: progressGlowWidthSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 8
                    value: AppSettings.progressGlowWidth
                    hoverEnabled: true
                    onMoved: AppSettings.progressGlowWidth = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowWidthSlider.hovered
                    }
                }
                TextField {
                    id: progressGlowWidthField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: progressGlowWidthSlider.from; top: progressGlowWidthSlider.to; decimals: 1 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.progressGlowWidth = Math.max(progressGlowWidthSlider.from, Math.min(progressGlowWidthSlider.to, v))
                        text = AppSettings.progressGlowWidth.toFixed(1)
                    }

                    background: UnderlineField { focused: progressGlowWidthField.activeFocus }

                    Binding {
                        target: progressGlowWidthField
                        property: "text"
                        value: AppSettings.progressGlowWidth.toFixed(1)
                        when: !progressGlowWidthField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowWidthField.hovered
                    }
                }
                Button {
                    id: progressGlowWidthResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("progressGlowWidth")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("progressGlowWidth")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: progressGlowWidthResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowWidthResetButton.enabled && progressGlowWidthResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: progressGlowEnabledSwitch.checked
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Glow radius")
                }
                Slider {
                    id: progressGlowRadiusSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 32
                    value: AppSettings.progressGlowRadius
                    hoverEnabled: true
                    onMoved: AppSettings.progressGlowRadius = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowRadiusSlider.hovered
                    }
                }
                TextField {
                    id: progressGlowRadiusField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: progressGlowRadiusSlider.from; top: progressGlowRadiusSlider.to; decimals: 1 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.progressGlowRadius = Math.max(progressGlowRadiusSlider.from, Math.min(progressGlowRadiusSlider.to, v))
                        text = AppSettings.progressGlowRadius.toFixed(1)
                    }

                    background: UnderlineField { focused: progressGlowRadiusField.activeFocus }

                    Binding {
                        target: progressGlowRadiusField
                        property: "text"
                        value: AppSettings.progressGlowRadius.toFixed(1)
                        when: !progressGlowRadiusField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowRadiusField.hovered
                    }
                }
                Button {
                    id: progressGlowRadiusResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("progressGlowRadius")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("progressGlowRadius")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: progressGlowRadiusResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowRadiusResetButton.enabled && progressGlowRadiusResetButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("Timestamps")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Decimal places")
                }
                Slider {
                    id: timestampDecimalPlacesSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 3
                    stepSize: 1
                    snapMode: Slider.SnapAlways
                    value: AppSettings.timestampDecimalPlaces
                    hoverEnabled: true
                    onMoved: AppSettings.timestampDecimalPlaces = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: timestampDecimalPlacesSlider.hovered
                    }
                }
                TextField {
                    id: timestampDecimalPlacesField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: IntValidator { bottom: timestampDecimalPlacesSlider.from; top: timestampDecimalPlacesSlider.to }
                    onEditingFinished: {
                        const v = parseInt(text, 10)
                        if (!isNaN(v))
                            AppSettings.timestampDecimalPlaces = Math.max(timestampDecimalPlacesSlider.from, Math.min(timestampDecimalPlacesSlider.to, v))
                        text = String(AppSettings.timestampDecimalPlaces)
                    }

                    background: UnderlineField { focused: timestampDecimalPlacesField.activeFocus }

                    Binding {
                        target: timestampDecimalPlacesField
                        property: "text"
                        value: String(AppSettings.timestampDecimalPlaces)
                        when: !timestampDecimalPlacesField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: timestampDecimalPlacesField.hovered
                    }
                }
                Button {
                    id: timestampDecimalPlacesResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("timestampDecimalPlaces")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("timestampDecimalPlaces")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: timestampDecimalPlacesResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: timestampDecimalPlacesResetButton.enabled && timestampDecimalPlacesResetButton.hovered
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Rounds the transcript gutter's displayed timestamp only — the underlying data always keeps full precision.")
                color: "#888888"
                font.pixelSize: 11
            }

            Label {
                text: qsTr("Files")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Show labels instead of filenames (main panel)")
                }
                Switch {
                    id: useLabelsMainPanelSwitch
                    checked: AppSettings.useLabelsMainPanel
                    hoverEnabled: true
                    onToggled: AppSettings.useLabelsMainPanel = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: useLabelsMainPanelSwitch.hovered
                    }
                }
                Button {
                    id: useLabelsMainPanelResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("useLabelsMainPanel")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("useLabelsMainPanel")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: useLabelsMainPanelResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: useLabelsMainPanelResetButton.enabled && useLabelsMainPanelResetButton.hovered
                    }
                }
            }

            Label {
                visible: !useLabelsMainPanelSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Off: renaming a file in the main panel renames the actual audio + transcript files instead of just setting a label.")
                color: "#888888"
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Show labels instead of filenames (sidebar)")
                }
                Switch {
                    id: useLabelsSidebarSwitch
                    checked: AppSettings.useLabelsSidebar
                    hoverEnabled: true
                    onToggled: AppSettings.useLabelsSidebar = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: useLabelsSidebarSwitch.hovered
                    }
                }
                Button {
                    id: useLabelsSidebarResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("useLabelsSidebar")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("useLabelsSidebar")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: useLabelsSidebarResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: useLabelsSidebarResetButton.enabled && useLabelsSidebarResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Hide file extensions (sidebar)")
                }
                Switch {
                    id: hideExtensionsSidebarSwitch
                    checked: AppSettings.hideExtensionsSidebar
                    hoverEnabled: true
                    onToggled: AppSettings.hideExtensionsSidebar = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: hideExtensionsSidebarSwitch.hovered
                    }
                }
                Button {
                    id: hideExtensionsSidebarResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("hideExtensionsSidebar")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("hideExtensionsSidebar")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: hideExtensionsSidebarResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: hideExtensionsSidebarResetButton.enabled && hideExtensionsSidebarResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Hide file extensions (main panel)")
                }
                Switch {
                    id: hideExtensionsMainPanelSwitch
                    checked: AppSettings.hideExtensionsMainPanel
                    hoverEnabled: true
                    onToggled: AppSettings.hideExtensionsMainPanel = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: hideExtensionsMainPanelSwitch.hovered
                    }
                }
                Button {
                    id: hideExtensionsMainPanelResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("hideExtensionsMainPanel")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("hideExtensionsMainPanel")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: hideExtensionsMainPanelResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: hideExtensionsMainPanelResetButton.enabled && hideExtensionsMainPanelResetButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("AI Auto-Labeling")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Auto-generate label, prelude, and tags")
                }
                Switch {
                    id: autoLabelEnabledSwitch
                    checked: AppSettings.autoLabelEnabled
                    hoverEnabled: true
                    onToggled: AppSettings.autoLabelEnabled = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: autoLabelEnabledSwitch.hovered
                    }
                }
                Button {
                    id: autoLabelEnabledResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("autoLabelEnabled")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("autoLabelEnabled")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: autoLabelEnabledResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: autoLabelEnabledResetButton.enabled && autoLabelEnabledResetButton.hovered
                    }
                }
            }

            Label {
                visible: autoLabelEnabledSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Runs a small local AI model (~2.5 GB download on first use) after each "
                           + "transcription to generate a display label, prelude, and tags.")
                color: "#888888"
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                visible: autoLabelEnabledSwitch.checked
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Only use existing tags (auto-categorize)")
                }
                Switch {
                    id: restrictTagsToExistingSwitch
                    checked: AppSettings.restrictTagsToExisting
                    hoverEnabled: true
                    onToggled: AppSettings.restrictTagsToExisting = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: restrictTagsToExistingSwitch.hovered
                    }
                }
                Button {
                    id: restrictTagsToExistingResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("restrictTagsToExisting")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("restrictTagsToExisting")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: restrictTagsToExistingResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: restrictTagsToExistingResetButton.enabled && restrictTagsToExistingResetButton.hovered
                    }
                }
            }

            Label {
                visible: autoLabelEnabledSwitch.checked && restrictTagsToExistingSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Off (default): freely invent new tags. On: only pick from tags already used "
                           + "elsewhere in this vault — leaves a file untagged rather than inventing one "
                           + "if nothing existing fits.")
                color: "#888888"
                font.pixelSize: 11
            }

            // Item 15 (Wave E): only meaningful when NOT hard-restricted —
            // restrictTagsToExisting above is already a hard rule, so this
            // gradient wouldn't have anything to adjust while it's on.
            RowLayout {
                Layout.fillWidth: true
                visible: autoLabelEnabledSwitch.checked && !restrictTagsToExistingSwitch.checked
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Tag reuse bias")
                }
                Slider {
                    id: tagReuseBiasSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 1
                    value: AppSettings.tagReuseBias
                    hoverEnabled: true
                    onMoved: AppSettings.tagReuseBias = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: tagReuseBiasSlider.hovered
                    }
                }
                Button {
                    id: tagReuseBiasResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("tagReuseBias")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("tagReuseBias")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: tagReuseBiasResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: tagReuseBiasResetButton.enabled && tagReuseBiasResetButton.hovered
                    }
                }
            }

            Label {
                visible: autoLabelEnabledSwitch.checked && !restrictTagsToExistingSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("How much to favor reusing an existing tag over inventing a new one. Low: "
                           + "prefer a precise new tag. High: stretch and reuse an existing tag even if "
                           + "it's only a loose fit.")
                color: "#888888"
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                visible: autoLabelEnabledSwitch.checked
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Label wordiness")
                }
                Slider {
                    id: wordinessSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 1
                    value: AppSettings.wordiness
                    hoverEnabled: true
                    onMoved: AppSettings.wordiness = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: wordinessSlider.hovered
                    }
                }
                Button {
                    id: wordinessResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("wordiness")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("wordiness")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: wordinessResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: wordinessResetButton.enabled && wordinessResetButton.hovered
                    }
                }
            }

            Label {
                visible: autoLabelEnabledSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("How verbose the AI-generated label and prelude are. Low: terse. High: "
                           + "descriptive.")
                color: "#888888"
                font.pixelSize: 11
            }
        }
    }
}
