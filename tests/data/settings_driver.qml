// Drives the REAL SettingsPopup for tst_icontheme's env-gated grabSettings
// slot (never runs in normal ctest) — so the icon-pack picker + per-icon
// override grid can be eyeballed as the user will actually see them, not just
// asserted. Null controllers: the Appearance page (page 0) never touches
// them; the combine page would, but this only ever shows page 0.
import QtQuick
import QtQuick.Controls
import SuopTranscriber

Item {
    id: root
    width: 720
    height: 760

    Rectangle { anchors.fill: parent; color: "#1a1a1a" }

    property alias popup: popup
    SettingsPopup {
        id: popup
        transcriptionController: null
        vaultTreeModel: null
        modal: false
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: Overlay.overlay
    }

    Component.onCompleted: {
        popup.currentPage = 0                 // Appearance page
        popup.iconOverridesExpanded = true    // show the full override grid
        popup.open()
    }
}
