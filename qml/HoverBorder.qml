import QtQuick
import SuopTranscriber

Rectangle {
    property bool active: false
    // Optional overrides so the same fade-in border can also render a stronger
    // "active/selected" ring (e.g. a top-level page's toolbar button while its
    // page is open) — default to the 1px accent hover border (StyleGuide
    // "Interactable hover behavior").
    property color borderColor: AppSettings.accentColor
    property int borderWidth: 1

    color: "transparent"
    radius: AppSettings.borderRadius
    border.width: borderWidth
    border.color: borderColor
    opacity: active ? 1 : 0

    Behavior on opacity {
        NumberAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 }
    }
}
