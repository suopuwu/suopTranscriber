import QtQuick
import SuopTranscriber

// Item 14 (Wave E): shared TextField background — a bottom underline instead
// of the default QQC2 boxed background, thicker/brighter (AppSettings
// highlightColor) when the field has focus. Used as every TextField's
// `background:` override so styling stays in one place.
Item {
    property bool focused: false

    implicitHeight: 24

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: focused ? 2 : 1
        color: focused ? AppSettings.highlightColor : "#5a5a5a"

        Behavior on height {
            NumberAnimation { duration: 100 }
        }
        Behavior on color {
            ColorAnimation { duration: 100 }
        }
    }
}
