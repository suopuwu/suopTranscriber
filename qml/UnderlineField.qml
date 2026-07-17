import QtQuick
import SuopTranscriber

// Item 14 (Wave E): shared TextField background — a thin white bottom
// underline instead of the default QQC2 boxed background, shown only while
// the field has focus. Used as every TextField's `background:` override so
// styling stays in one place.
Item {
    property bool focused: false
    // Opt out of the resting underline: set on the always-editable fields
    // (StyleGuide.md "Always-editable text" — title/tags/speakers/prelude/
    // transcript), whose whole point is to read as prose rather than as a
    // permanently-visible input box. A pure text box — one that exists only
    // to be typed into (filename regex, start folder, search, the numeric
    // settings fields) — has no such prose to blend into, so it keeps its
    // resting underline and always looks like the input it is.
    property bool plainUntilHovered: false

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
        // Three tiers, each dimmer than the state above it, so hover still
        // reads as a distinct step on both kinds of field: focused (1) >
        // hovered (0.5) > at rest (0.25, or hidden entirely when
        // plainUntilHovered). Same fade idiom as HoverBorder's own opacity
        // animation.
        opacity: focused ? 1 : (hoverHandler.hovered ? 0.5 : (plainUntilHovered ? 0 : 0.25))

        Behavior on opacity {
            NumberAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 }
        }
    }
}
