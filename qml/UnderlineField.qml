import QtQuick
import SuopTranscriber

// Item 14 (Wave E): shared TextField background — a thin white bottom
// underline instead of the default QQC2 boxed background, shown only while
// the field has focus. Used as every TextField's `background:` override so
// styling stays in one place.
Item {
    property bool focused: false

    implicitHeight: 24

    HoverHandler {
        id: hoverHandler
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: AppSettings.accentColor
        // Fully hidden until focused — an always-editable field (see
        // StyleGuide.md "Always-editable text") must read as plain text the
        // rest of the time, not a permanently-visible input box. Same
        // fade idiom as HoverBorder's own opacity animation. A dim (50%)
        // preview appears on hover so these fields (tags/speakers/prelude/
        // title) hint they're editable before the user commits to a click.
        opacity: focused ? 1 : (hoverHandler.hovered ? 0.5 : 0)

        Behavior on opacity {
            NumberAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 }
        }
    }
}
