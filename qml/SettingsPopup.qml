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
    // Non-modal: a modal Popup's input-blocking layer covers the ENTIRE
    // window via Overlay.overlay, including the window-edge ResizeGrips
    // (Main.qml) — that would silently eat every resize attempt while the
    // popup was open. `dim` is set explicitly below (not left to follow
    // `modal`), and closePolicy already closes on an outside press, so the
    // popup still looks and dismisses the same way. Item 3 (Wave E):
    // interaction with the rest of the window IS still blocked while this is
    // open — via a separate input-swallowing MouseArea in Main.qml, declared
    // below the ResizeGrips in z-order so those alone stay usable, rather
    // than through this Popup's own modal/Overlay mechanism.
    modal: false
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
                || !AppSettings.isDefault("useLabelsMainPanel")
                || !AppSettings.isDefault("useLabelsSidebar")
                || !AppSettings.isDefault("hideExtensionsSidebar")
                || !AppSettings.isDefault("hideExtensionsMainPanel")
                || !AppSettings.isDefault("autoLabelEnabled")
                || !AppSettings.isDefault("restrictTagsToExisting")
                || !AppSettings.isDefault("tagReuseBias")
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
                    text: qsTr("Auto-generate label, summary, and tags")
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
                           + "transcription to generate a display label, summary, and tags.")
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
        }
    }
}
