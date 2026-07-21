import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Base for every top-level screen. Floats its vertical scrollbar in the
// window-edge gutter (reclaimed dead margin) instead of over content, and bakes
// in Chrome-style middle-click autoscroll. Callers anchors.fill this and drop
// their scrolling content inside as the single child (default `content` alias);
// width-filling content binds to availableContentWidth so the bar sits in
// reclaimed space, not on text. The transcript note page is the reference
// implementation (its sticky rows live in the `header` slot, its transcribing
// spinner in `bodyOverlay`); benchmark/USB/recorder are thinner instances that
// use only `content`. See MiddleDragScroll.qml for the middle-drag trick, and
// note why MiddleDragScroll is a SIBLING of the ScrollView, never a child
// (ScrollView mis-tracks content size with >1 content child) — likewise why
// bodyOverlay siblings the ScrollView inside the body region rather than living
// inside the scroll body.
Item {
    id: root

    // Sticky content, fixed ABOVE the scroll body (never scrolls). Empty for
    // benchmark/USB/recorder, so their look is unchanged. Routed to a ColumnLayout,
    // so header children stack vertically and use Layout.* attached props.
    property alias header: headerSlot.data

    // Caller's scrolling content — the ScrollView's SINGLE child. Aliased to
    // contentData (ScrollView's own default property), NOT the Control's plain
    // `data` list — only contentData routes the child into the flickable.
    default property alias content: bodyScroll.contentData

    // Siblings of the ScrollView inside the body region (e.g. the transcribing
    // spinner overlay) — anchors.fill: parent covers exactly the body, not the
    // header. NEVER put these inside bodyScroll: the ScrollView must keep exactly
    // ONE content child or it mis-tracks content size.
    property alias bodyOverlay: bodyContainer.data

    // The inner ScrollView, exposed so callers can reach .contentItem (the
    // Flickable, for scroll math) and .availableWidth.
    property alias bodyScrollView: bodyScroll

    // Convenience for harnesses/tests that look the inner ScrollView up by
    // objectName (the transcript viewport contract).
    property alias bodyObjectName: bodyScroll.objectName

    // Basic-style vertical ScrollBar overlay clearance. Width-filling content
    // binds here so it keeps its inset and the bar floats in the gutter.
    readonly property int scrollBarGutter: 13
    readonly property real availableContentWidth: bodyScroll.availableWidth - scrollBarGutter

    ColumnLayout {
        anchors.fill: parent
        // 16px inset like every page, EXCEPT the right edge: shrink it so the
        // ScrollView reaches into the gutter and the overlay bar lands ~3px
        // from the panel edge (16 - scrollBarGutter). Content bound to
        // availableContentWidth keeps its true 16px right inset.
        anchors.leftMargin: 16
        anchors.topMargin: 16
        anchors.bottomMargin: 16
        anchors.rightMargin: 16 - root.scrollBarGutter
        // 12px header/body gap — matches the note page's old header-to-transcript
        // spacing. Only ever applied when the header slot has children (it's
        // collapsed out of the layout otherwise, so header-less pages are unshifted).
        spacing: 12

        // Sticky header. Excluded from the layout (visible:false) when empty so
        // pages without a header look byte-identical to a bare ScrollView — no
        // stray 12px spacing above the body. The header keeps the FULL width
        // (it isn't scrolled, so it doesn't cede the gutter): its own right
        // padding gives back the gutter the outer ColumnLayout bled off.
        ColumnLayout {
            id: headerSlot
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.rightMargin: root.scrollBarGutter
            visible: headerSlot.children.length > 0
            spacing: 12
        }

        // Body region: the ScrollView plus any bodyOverlay siblings on top.
        Item {
            id: bodyContainer
            Layout.fillWidth: true
            Layout.fillHeight: true

            ScrollView {
                id: bodyScroll
                anchors.fill: parent
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            }
        }
    }

    // Sibling overlay (NOT inside bodyScroll). Armed only while this page shows.
    MiddleDragScroll {
        anchors.fill: parent
        flickable: bodyScroll.contentItem as Flickable
        visible: root.visible
    }
}
