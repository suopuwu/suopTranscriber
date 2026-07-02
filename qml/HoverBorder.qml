import QtQuick
import SuopTranscriber

Rectangle {
    property bool active: false

    color: "transparent"
    radius: AppSettings.borderRadius
    border.width: 1
    border.color: "white"
    opacity: active ? 1 : 0

    Behavior on opacity {
        NumberAnimation { duration: 100 }
    }
}
