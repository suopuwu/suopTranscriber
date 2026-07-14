import QtQuick
import SuopTranscriber

Rectangle {
    property bool active: false

    color: "transparent"
    radius: AppSettings.borderRadius
    border.width: 1
    border.color: AppSettings.accentColor
    opacity: active ? 1 : 0

    Behavior on opacity {
        NumberAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 }
    }
}
