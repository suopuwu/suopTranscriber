import QtQuick
import QtQuick.Controls

// Chrome/Firefox-style middle-click autoscroll, as a drop-in overlay: give it
// a `flickable` to scroll and anchor/size it over the region a middle-click
// should ARM from (the transcript viewport, the file tree, the settings scroll
// area). One component, three call sites — MainPanel, Sidebar and
// SettingsPopup each used to carry their own near-identical copy, and each copy
// independently had to relearn the same Qt Quick facts recorded below.
//
// TWO GESTURES, both ending in the SAME free-scroll state:
//   1. A quick click (press+release, no real drag) arms autoscroll, which then
//      free-scrolls just from MOVING the mouse — no button held — until
//      another click (any button) or Escape disarms it.
//   2. Press-hold-drag past a small threshold, then release: disarms
//      immediately on release, no second click needed.
// draggedWhileHeld tells the two apart — it only flips true once real movement
// exceeds the deadzone WHILE the button is down, so a plain click that never
// moved makes release a no-op (armed stays true), which is what preserves
// gesture 1.
//
// ONCE ARMED, THE GESTURE BELONGS TO THE WHOLE WINDOW, NOT TO THIS OVERLAY —
// that's what `tracker` below is for, and it's the whole reason the armed
// phase isn't just handled here. Qt Quick hover delivery and pointer grabs are
// both bounds-scoped per item, so an overlay that only spans its own panel
// froze the scroll the moment the cursor wandered onto anything else (the
// sidebar, the bottom bar, the title bar, another panel's content) — Chrome
// keeps scrolling wherever the cursor goes, including outside the window.
// Widening this item itself is not an option: it must stay narrow so that
// middle-clicking a DIFFERENT panel arms THAT panel's autoscroll, not this
// one. So arming stays panel-scoped and only the armed phase goes window-wide.
//
// NOT a MouseArea, and NOT z-toggled — both were tried in earlier versions and
// both broke something real, confirmed empirically each time (offscreen QTest):
//   1. A statically-topmost MouseArea caught the click but stole the cursor
//      from every hoverable element underneath, even fully idle — Qt Quick's
//      cursor resolution is a pure top-down paint-order scan for the topmost
//      HIT item, independent of hoverEnabled/acceptedButtons, and a MouseArea
//      always implicitly claims a cursor (default ArrowCursor) merely by being
//      that item, regardless of its own config.
//   2. Toggling that MouseArea's z below (-1) while idle fixed the cursor but
//      broke the click itself over a ScrollView: unlike a bare Flickable
//      (which claims only LeftButton and lets MiddleButton fall through),
//      QQC2's ScrollView Control claims ALL mouse-button presses within its
//      bounds at the outer Control level, before any per-button filtering, so
//      a lower-z sibling never sees a middle-click over it at all.
// The actual fix: a plain Item has NO implicit cursor, so it can sit
// permanently topmost without poisoning cursor resolution underneath — as long
// as it never binds an ENABLED cursor-setting handler while idle (a DISABLED
// HoverHandler{cursorShape:...} doesn't count, which is why `tracker`'s can be
// left bound). A permanently-topmost TapHandler still catches the middle press
// through a ScrollView's swallow-everything Control layer and through a
// selectByMouse TextEdit's exclusive grab, because a TapHandler participates
// in per-button hit-testing the way MouseArea's fallthrough does — it just
// doesn't drag the implicit-cursor baggage along.
Item {
    id: root

    // The Flickable to drive. A TreeView/ListView IS one; for a ScrollView
    // pass `theScrollView.contentItem as Flickable` — contentY/contentHeight/
    // height live on the ScrollView's internal Flickable, not on the
    // ScrollView itself.
    required property Flickable flickable

    // True while autoscroll is live (either gesture). Read-only to callers —
    // SettingsPopup suspends its own closePolicy on it, since Escape and
    // click-outside are how this cancels itself too.
    property bool armed: false

    // Radius around the arm point where nothing scrolls at all.
    readonly property int deadzone: 8

    // Scene coordinates, NOT local: the armed phase tracks the pointer from
    // `tracker` (a different item, in a different part of the scene), so the
    // two only agree on a shared coordinate space. Only their delta is ever
    // used, so the choice of space just has to be consistent.
    property real originY: 0
    property real currentY: 0
    property bool draggedWhileHeld: false

    z: 1

    // Callers gate this overlay on the target actually being on screen
    // (MainPanel's note page, Sidebar's unfiltered tree). Going away
    // mid-gesture must take the armed phase — which lives on `tracker`, out in
    // the window overlay where it inherits neither — down with it.
    onVisibleChanged: if (!root.visible) root.armed = false
    onEnabledChanged: if (!root.enabled) root.armed = false

    // Arms both gestures, and tracks gesture 2 for as long as the button is
    // held. gesturePolicy matters twice over:
    //   - The default (DragThreshold) cancels TapHandler's own grab — and
    //     stops delivering ANY further events to anyone, including the real
    //     release — the instant the drag passes the platform threshold. That
    //     is far too early for gesture 2's long hold-drag.
    //   - DragWithinBounds (the previous choice) survives the drag but still
    //     cancels the moment the pointer leaves THIS item, which is exactly
    //     the "scroll dies when you drag onto another element" bug.
    // ReleaseWithinBounds cancels on neither: it takes the exclusive grab on
    // press and holds it until the real release, wherever that lands, so
    // point.scenePosition keeps updating across the whole window. (Its "within
    // bounds" clause only decides whether `tapped` is emitted — a signal this
    // component never uses.)
    TapHandler {
        id: armTap
        acceptedButtons: Qt.MiddleButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onPressedChanged: {
            if (armTap.pressed) {
                root.draggedWhileHeld = false
                root.armed = true
                root.originY = armTap.point.scenePosition.y
                root.currentY = armTap.point.scenePosition.y
            } else if (root.draggedWhileHeld) {
                root.armed = false
                // else: real click, no drag — gesture 1 keeps running.
            }
        }
        onPointChanged: {
            if (!armTap.pressed)
                return
            root.currentY = armTap.point.scenePosition.y
            if (!root.draggedWhileHeld
                    && Math.abs(root.currentY - root.originY) > root.deadzone) {
                root.draggedWhileHeld = true
            }
        }
    }

    // The armed phase, hoisted to the window overlay so it spans the entire
    // window instead of just the arming region: it drives gesture 1's
    // free-scroll tracking, the armed cursor, and click-to-cancel — all three
    // of which should follow the pointer anywhere, per Chrome.
    //
    // Overlay.overlay fills the ApplicationWindow's contentItem (same
    // coordinate space as the window's root content) and is created on demand
    // for any window, so this works in test harnesses too. z mirrors Main.qml's
    // ResizeGrip precedent — above SettingsPopup's modal block-plane (and above
    // the grips themselves), so an armed scroll keeps tracking over a popup
    // rather than dying at its edge.
    //
    // Both gates matter: `enabled` alone would leave it swallowing hover, and
    // an invisible item receives no events at all. While idle it is therefore
    // fully inert and contributes no cursor — the disabled-HoverHandler rule
    // in the header comment.
    Item {
        id: tracker
        objectName: "middleDragTracker"
        // `root.Overlay.overlay`, NOT a bare `Overlay.overlay` — that reads as
        // "the overlay of the window TRACKER is in", and this binding is the
        // very thing that takes tracker out of root's subtree, so it would
        // deadlock: the first evaluation (before this item ever has a window,
        // e.g. while the scene is still being built off-window) resolves to
        // null, that null unparents tracker, an unparented item never gains a
        // window, so windowChanged never fires and the binding never
        // re-evaluates — leaving the tracker permanently parentless, sizeless
        // and inert. Resolving it against `root` — which is in the scene, and
        // stays there — makes the binding re-evaluate the moment root gets its
        // window. Verified: with the bare form, the tracker really does end up
        // parent=null/0x0 forever (see tst_middledragscroll).
        parent: root.Overlay.overlay
        anchors.fill: parent
        z: 1100
        enabled: root.armed
        visible: root.armed

        HoverHandler {
            id: trackHover
            // SizeAll inside the deadzone (any direction still possible),
            // SizeVer once real scrolling starts — same threshold the timer
            // uses, so the cursor flips exactly when the content does. Qt has
            // no single-axis up/down cursor; these are the closest built-ins.
            cursorShape: Math.abs(root.currentY - root.originY) <= root.deadzone
                         ? Qt.SizeAllCursor : Qt.SizeVerCursor
            onPointChanged: {
                // Gesture 1 only: while the button is held, armTap's grab is
                // the authority on where the pointer is.
                if (root.armed && !armTap.pressed)
                    root.currentY = trackHover.point.scenePosition.y
            }
        }

        // A left/middle click anywhere while armed cancels, and is consumed
        // rather than performing that button's normal action (Chrome's
        // convention). Being topmost, this beats the click to every other
        // handler in the window — including armTap, which therefore never has
        // to handle the already-armed case itself.
        TapHandler {
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onPressedChanged: if (pressed) root.armed = false
        }

        // Right-click while armed ALSO cancels — but must NOT be swallowed.
        // The app opens its file-tree / transcript context menus from a
        // RightButton TapHandler UNDER this window-wide tracker, so consuming
        // the press here (as the left/middle handler above does) is exactly the
        // "right-click does nothing while autoscroll is invisibly armed" bug:
        // a plain middle-click arms gesture 1 and leaves it armed until the
        // next click, and because the tracker spans the whole window (across
        // all three MiddleDragScroll instances), arming a scroll in ANY pane
        // then ate the next right-click everywhere. DragThreshold keeps only a
        // PASSIVE grab on the press, so onPressedChanged still fires to disarm,
        // yet the press also propagates down to the context-menu handler.
        // Disarming on press immediately makes this tracker inert
        // (enabled/visible:false), so it can't then eat the menu's own clicks.
        TapHandler {
            acceptedButtons: Qt.RightButton
            gesturePolicy: TapHandler.DragThreshold
            onPressedChanged: if (pressed) root.armed = false
        }
    }

    // Repeating timer, gated entirely by `armed` — it never runs while idle, so
    // it's no more a background poll than a Flickable's own flick-deceleration
    // animation. Quadratic and uncapped: dragging farther from the origin keeps
    // accelerating rather than plateauing at some max speed, and the content-
    // size clamp below is what actually stops it.
    Timer {
        interval: 16
        repeat: true
        running: root.armed
        onTriggered: {
            const dy = root.currentY - root.originY
            if (Math.abs(dy) <= root.deadzone)
                return
            const beyond = dy - Math.sign(dy) * root.deadzone
            const speed = Math.sign(beyond) * beyond * beyond * 0.002
            const maxY = Math.max(0, root.flickable.contentHeight - root.flickable.height)
            root.flickable.contentY = Math.max(0, Math.min(maxY, root.flickable.contentY + speed))
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.armed
        onActivated: root.armed = false
    }
}
