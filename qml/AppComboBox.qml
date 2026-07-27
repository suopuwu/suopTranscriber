import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// Reusable eliding combo box shared by the MainPanel model pickers (the
// transcription model plus the diarization segmentation/embedding models)
// and the Recorder page's mic/system-source device pickers.
//
// - The closed box shows a TRUNCATED label for the current selection (full
//   text stays in the hover tooltip and in the wider dropdown popup), so a
//   long label never overflows a width-capped row.
// - Both the closed box and the popup round their corners with
//   AppSettings.borderRadius (QQC2's default ComboBox box/popup are
//   square-cornered) — StyleGuide.md border-radius convention.
// - Each entry carries a small status dot, but only when it has one: green if
//   that model's weights are already on disk, gray if picking it will first
//   queue a download job. Device entries have no `downloaded` field, so the
//   dot is simply absent for them.
//
// Every model list this drives (availableModels() / availableSegmentationModels()
// / availableEmbeddingModels()) exposes {id, label, downloaded}; the device
// lists (availableMicDevices() / systemSources) expose {id, name} instead —
// labelRole below picks which field is the display text.
ComboBox {
    id: control

    // The currently-selected entry id (valueRole). Kept in sync with the
    // dropdown's currentIndex; assigning it externally moves the selection.
    property string selectedId: ""

    // Which field of an entry is its display text. Defaults to the model
    // lists' `label`; the recorder's device lists spell the same thing `name`.
    property string labelRole: "label"

    // Emitted only when the USER picks an entry (not on programmatic
    // selectedId changes) — the parent persists this to the relevant
    // AppSettings key.
    signal picked(string id)

    // Widest label's advance width, measured once per model change so the
    // popup can open wide enough to show every label in full (the closed box
    // stays capped and elides).
    property real cachedMaxLabelWidth: 0

    textRole: control.labelRole
    valueRole: "id"
    hoverEnabled: true
    // Reading `count` is load-bearing, not cosmetic: indexOfValue() reads the
    // model internally, but that read is NOT a tracked binding dependency, so
    // when the model list is still empty as this combo is built (e.g. the
    // controller/model assigned a tick later) the binding would compute -1 once
    // and never re-run when the list fills in — leaving the box stuck empty.
    // Touching `count` ties this binding to the list changing so it recomputes.
    // And when selectedId isn't in the list (nothing chosen yet, or a stale/
    // removed id) fall back to the FIRST entry rather than showing nothing.
    function resolveIndex() {
        const n = count
        const i = indexOfValue(selectedId)
        return i >= 0 ? i : (n > 0 ? 0 : -1)
    }
    currentIndex: control.resolveIndex()

    onActivated: control.picked(currentValue)

    function recomputeMaxLabelWidth() {
        let maxWidth = 0
        for (let i = 0; i < count; i++)
            maxWidth = Math.max(maxWidth, modelLabelFontMetrics.advanceWidth(textAt(i)))
        control.cachedMaxLabelWidth = maxWidth
    }
    // QQuickComboBox::setModel() force-resets currentIndex ("0 whenever
    // count > 0") when the list is assigned after the combo is complete —
    // exactly how MainPanel drives these (empty array at build time, real list
    // in Component.onCompleted and again on modelsChanged). That C++ write
    // lands after the binding above last ran, and a same-length replacement
    // never changes `count`, so without re-asserting here the box silently
    // shows the FIRST model while selectedId still holds the saved one.
    // modelChanged is emitted last, so this runs after the reset.
    onModelChanged: {
        recomputeMaxLabelWidth()
        currentIndex = Qt.binding(control.resolveIndex)
    }
    Component.onCompleted: recomputeMaxLabelWidth()

    // A single throwaway FontMetrics reused across the whole list (not one
    // per delegate) to size the popup to the widest label.
    FontMetrics {
        id: modelLabelFontMetrics
    }

    ToolTip.visible: control.hovered
    ToolTip.text: control.currentText
    ToolTip.delay: 500

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
        enabled: control.enabled
    }
    HoverBorder {
        anchors.fill: parent
        active: control.enabled && control.hovered
    }

    // Custom eliding content: the default QQC2 ComboBox contentItem clips (no
    // ellipsis) when the box is width-capped, so provide our own that elides.
    contentItem: Text {
        leftPadding: 12
        rightPadding: 28
        text: control.displayText
        color: "white"
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    // Closed-state box gets the app's own corner radius (QQC2's default box is
    // square-cornered), matching the popup background below.
    background: Rectangle {
        color: Theme.surfaceDialog
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: Theme.border
    }

    // Small dot per entry — green if the weights are on disk, gray if picking
    // it will first queue a download.
    delegate: ItemDelegate {
        id: modelDelegate
        required property var modelData
        required property int index
        // Follow the popup's width (not capped like the closed box).
        width: ListView.view ? ListView.view.width : control.width
        highlighted: control.highlightedIndex === index

        contentItem: RowLayout {
            spacing: 6
            Label {
                text: modelDelegate.modelData[control.labelRole]
                color: "white"
                Layout.fillWidth: true
                // Wrap instead of eliding: when the popup is clamped by the
                // screen edge the label spills onto a second line rather than
                // being cut.
                wrapMode: Text.Wrap
            }
            // Download-state dot — only for lists that HAVE a download state.
            // An audio device is never "not downloaded", and a dot that is
            // always the same color is noise.
            Rectangle {
                visible: modelDelegate.modelData.downloaded !== undefined
                width: 8
                height: 8
                radius: 4
                color: modelDelegate.modelData.downloaded ? Theme.success : "#555555"
                Layout.alignment: Qt.AlignVCenter
            }
        }
    }

    // Custom popup so its background can use the app's own corner radius
    // (matches SettingsPopup.qml's own Popup background) instead of QQC2's
    // default square-cornered popup. The ListView still drives itself off
    // control.delegateModel (built from the `model:`/`delegate:` above) — the
    // same way QQC2's own default ComboBox popup composes internally.
    popup: Popup {
        id: modelPopup
        y: control.height
        padding: 1

        // Unlike the closed box, the popup is not width-capped — it opens as
        // wide as the widest label needs so nothing is clipped. It's only
        // clamped to the space left to the window's right edge; when even that
        // isn't enough, the delegate labels wrap.
        property real desiredWidth: Math.max(control.width, control.cachedMaxLabelWidth + 64)
        width: desiredWidth
        onAboutToShow: {
            const p = control.mapToItem(null, 0, 0)
            const avail = control.Window.width - p.x - 12
            width = Math.min(desiredWidth, Math.max(control.width, avail))
        }

        implicitHeight: Math.min(contentItem.implicitHeight, 320)

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }

        background: Rectangle {
            color: Theme.surfaceDialog
            radius: AppSettings.borderRadius
            border.width: 1
            border.color: Theme.border
        }
    }
}
