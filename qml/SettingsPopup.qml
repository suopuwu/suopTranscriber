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
    // (Main.qml) — that silently ate every resize attempt while the popup
    // was open. `dim` is set explicitly below (not left to follow `modal`),
    // and closePolicy already closes on an outside press, so the popup still
    // looks and dismisses the same way; it just no longer blocks the rest of
    // the window's input.
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
                || !AppSettings.isDefault("useLabels")
                || !AppSettings.isDefault("hideExtensionsSidebar")
                || !AppSettings.isDefault("hideExtensionsMainPanel")
                || !AppSettings.isDefault("autoLabelEnabled")
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
                        color: resetAllButton.hovered ? "#3a3a3a" : "#2a2a2a"
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
                        color: changeVaultButton.hovered ? "#3a3a3a" : "#2a2a2a"
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
                        color: sidebarColorButton.hovered ? "#3a3a3a" : "#2a2a2a"
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
                        color: mainColorButton.hovered ? "#3a3a3a" : "#2a2a2a"
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
                        color: rimColorButton.hovered ? "#3a3a3a" : "#2a2a2a"
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
                text: qsTr("Files")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Show labels instead of filenames")
                }
                Switch {
                    id: useLabelsSwitch
                    checked: AppSettings.useLabels
                    hoverEnabled: true
                    onToggled: AppSettings.useLabels = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: useLabelsSwitch.hovered
                    }
                }
                Button {
                    id: useLabelsResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("useLabels")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("useLabels")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: useLabelsResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: useLabelsResetButton.enabled && useLabelsResetButton.hovered
                    }
                }
            }

            Label {
                visible: !useLabelsSwitch.checked
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
        }
    }
}
