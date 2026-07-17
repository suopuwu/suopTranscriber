import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SuopTranscriber

// Settings "Models" page (mounted by SettingsPopup as page 5). Lists every
// app-managed model — the transcription tiers + Parakeet, and the support
// models (auto-label / VAD / diarization) — showing which are downloaded and
// their on-disk size, with per-model Download (predownload), Delete, Export,
// and Import. Models live in ONE app-global store shared across vaults
// (ModelManager); a broken download URL is survivable by importing a
// backed-up file, or by a distributor dropping models in the bundled dir. See
// DESIGN.md "Model management".
ColumnLayout {
    id: modelsPage

    property var transcriptionController: null

    spacing: 10

    // manageableModels() is a plain Q_INVOKABLE with no NOTIFY, so it's read
    // IMPERATIVELY (same untracked-invokable trap BenchmarkPage documents), on
    // show and whenever the controller signals a change.
    property var models: []
    function refresh() {
        modelsPage.models = modelsPage.transcriptionController
                ? modelsPage.transcriptionController.manageableModels() : []
    }

    // Import/export run on a background thread and answer via
    // modelOperationFinished — track the in-flight op per id to show a busy
    // state and disable that row's buttons meanwhile.
    property var pendingOp: ({})
    function setPending(id, op) {
        var p = modelsPage.pendingOp
        p[id] = op
        modelsPage.pendingOp = p
    }
    function clearPending(id) {
        var p = modelsPage.pendingOp
        delete p[id]
        modelsPage.pendingOp = p
    }

    // The model id a file/folder dialog is currently acting on.
    property string dialogModelId: ""
    property string pendingDeleteId: ""
    property string pendingDeleteLabel: ""

    onVisibleChanged: if (visible) modelsPage.refresh()
    Component.onCompleted: modelsPage.refresh()

    Connections {
        target: modelsPage.transcriptionController
        enabled: modelsPage.transcriptionController !== null

        // A predownload landing (or any download) flips a downloaded flag.
        function onModelsChanged() { modelsPage.refresh() }

        function onModelOperationFinished(operation, modelId, ok, error) {
            modelsPage.clearPending(modelId)
            if (!ok) {
                Notifications.error(qsTr("Couldn't %1 that model: %2").arg(operation).arg(error))
            } else if (operation === "export") {
                // Export leaves no visible change on the row, so confirm it.
                Notifications.info(qsTr("Model exported."))
            }
            // delete/import success speak for themselves — the row updates.
        }
    }

    Label {
        text: qsTr("Models")
        color: "white"
        font.bold: true
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("Downloaded models are shared across all vaults, stored once on this machine. "
                   + "Predownload one you'll want later, delete ones you don't, or export a copy "
                   + "to back it up — so a broken download link can't leave you stuck (re-import the file).")
        color: "#aaaaaa"
        font.pixelSize: 12
    }

    // --- Transcription models -------------------------------------------------
    Label {
        text: qsTr("Transcription")
        color: "#cccccc"
        font.pixelSize: 12
        font.bold: true
        Layout.topMargin: 4
    }
    Repeater {
        model: modelsPage.models.filter(function (m) { return m.kind === "transcription" })
        delegate: modelRow
    }

    // --- Support models -------------------------------------------------------
    Label {
        text: qsTr("Support models")
        color: "#cccccc"
        font.pixelSize: 12
        font.bold: true
        Layout.topMargin: 8
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("Downloaded automatically when their feature first runs (auto-labeling, "
                   + "chunking, speaker detection). Manage them here too.")
        color: "#888888"
        font.pixelSize: 11
    }
    Repeater {
        model: modelsPage.models.filter(function (m) { return m.kind === "support" })
        delegate: modelRow
    }

    // --- One model row --------------------------------------------------------
    Component {
        id: modelRow
        RowLayout {
            id: row
            required property var modelData
            Layout.fillWidth: true
            spacing: 8

            readonly property bool busy: modelsPage.pendingOp[modelData.id] !== undefined

            // Status dot (a Rectangle circle — not an icon, per StyleGuide):
            // green = on disk, gray = not downloaded.
            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: 10
                implicitHeight: 10
                radius: 5
                color: row.modelData.downloaded ? "#2ecc71" : "#555555"
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    Layout.fillWidth: true
                    text: row.modelData.label
                    color: "white"
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
                Label {
                    visible: row.modelData.downloaded && row.modelData.sizeText !== ""
                    text: row.modelData.bundledOnly
                          ? qsTr("%1 · bundled (read-only)").arg(row.modelData.sizeText)
                          : row.modelData.sizeText
                    color: "#888888"
                    font.pixelSize: 11
                }
            }

            // Busy label while an import/export runs for this row.
            Label {
                visible: row.busy
                text: modelsPage.pendingOp[row.modelData.id] === "export"
                      ? qsTr("Exporting…") : qsTr("Importing…")
                color: "#cccccc"
                font.pixelSize: 11
            }

            // Download (predownload) — only when not on disk.
            ModelActionButton {
                visible: !row.modelData.downloaded && !row.busy
                text: qsTr("Download")
                onClicked: {
                    modelsPage.transcriptionController.downloadModel(row.modelData.id)
                    Notifications.info(qsTr("Downloading — see Tasks for progress."))
                }
            }

            // Import a user-supplied copy (file or folder by shape).
            ModelActionButton {
                visible: !row.busy
                text: qsTr("Import…")
                onClicked: {
                    modelsPage.dialogModelId = row.modelData.id
                    if (row.modelData.multiFile)
                        importFolderDialog.open()
                    else
                        importFileDialog.open()
                }
            }

            // Export a backup — only when downloaded.
            ModelActionButton {
                visible: row.modelData.downloaded && !row.busy
                text: qsTr("Export…")
                onClicked: {
                    modelsPage.dialogModelId = row.modelData.id
                    exportFolderDialog.open()
                }
            }

            // Delete — only for something removable from the writable store
            // (never a bundled-only model).
            ModelActionButton {
                visible: row.modelData.downloaded && !row.modelData.bundledOnly && !row.busy
                text: qsTr("Delete")
                destructive: true
                onClicked: {
                    modelsPage.pendingDeleteId = row.modelData.id
                    modelsPage.pendingDeleteLabel = row.modelData.label
                    deleteConfirmPopup.open()
                }
            }
        }
    }

    // --- Native pickers -------------------------------------------------------
    FileDialog {
        id: importFileDialog
        title: qsTr("Choose the model file to import")
        onAccepted: {
            modelsPage.setPending(modelsPage.dialogModelId, "import")
            modelsPage.transcriptionController.importModel(modelsPage.dialogModelId, selectedFile)
        }
    }
    FolderDialog {
        id: importFolderDialog
        title: qsTr("Choose the folder containing the model files")
        onAccepted: {
            modelsPage.setPending(modelsPage.dialogModelId, "import")
            modelsPage.transcriptionController.importModel(modelsPage.dialogModelId, selectedFolder)
        }
    }
    FolderDialog {
        id: exportFolderDialog
        title: qsTr("Choose where to export the model")
        onAccepted: {
            modelsPage.setPending(modelsPage.dialogModelId, "export")
            modelsPage.transcriptionController.exportModel(modelsPage.dialogModelId, selectedFolder)
        }
    }

    // --- Delete confirmation (a model delete is permanent — re-download may
    // fail, which is exactly the risk the user worried about — so unlike a
    // vault file it is NOT trashed, and IS confirmed). -------------------------
    Popup {
        id: deleteConfirmPopup
        anchors.centerIn: Overlay.overlay
        modal: true
        padding: 16
        background: Rectangle {
            color: "#242424"
            radius: AppSettings.borderRadius
            border.width: 1
            border.color: "#3a3a3a"
        }
        ColumnLayout {
            spacing: 12
            Label {
                Layout.maximumWidth: 320
                wrapMode: Text.WordWrap
                text: qsTr("Delete \"%1\"? This removes the files from disk. "
                           + "You can download or import it again later.").arg(modelsPage.pendingDeleteLabel)
                color: "white"
                font.pixelSize: 12
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                ModelActionButton {
                    text: qsTr("Cancel")
                    onClicked: deleteConfirmPopup.close()
                }
                ModelActionButton {
                    text: qsTr("Delete")
                    destructive: true
                    onClicked: {
                        modelsPage.transcriptionController.deleteModel(modelsPage.pendingDeleteId)
                        deleteConfirmPopup.close()
                    }
                }
            }
        }
    }

    // Small labeled button matching BenchmarkPage's controls (hoverEnabled +
    // PointingHandCursor + HoverBorder per StyleGuide). Inlined here as a nested
    // component so the page stays one file.
    component ModelActionButton: Button {
        id: actionButton
        property bool destructive: false
        font.pixelSize: 11
        padding: 6
        contentItem: Label {
            text: actionButton.text
            font: actionButton.font
            color: actionButton.destructive ? "#e07a7a" : "#dddddd"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: AppSettings.borderRadius
            color: actionButton.hovered ? "#3a3a3a" : "#2a2a2a"
            border.width: 1
            border.color: "#3a3a3a"
        }
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        HoverBorder { anchors.fill: parent; active: actionButton.hovered }
    }
}
