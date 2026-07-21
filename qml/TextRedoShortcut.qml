import QtQuick
import QtQuick.Window

// Window-wide Ctrl+Y => redo in whatever text box currently has focus.
//
// Qt's built-in TextInput/TextEdit editing binds redo to QKeySequence::Redo,
// which on Linux is Ctrl+Shift+Z ONLY — Ctrl+Y is the Windows chord and is
// never mapped here, so a focused TextField/TextArea silently ignored it.
// This fills that gap for every text box in the app at once (transcript,
// chapter titles, speaker names, sidebar rename, settings, benchmark) rather
// than a per-field Keys.onPressed in a dozen files.
//
// Deliberately NOT ambiguous with MainPanel.qml's own Ctrl+Y (the panel-level
// label/prelude/transcript redo stack): that one is gated on
// mainPanel.activeFocus, and mainPanel's root is a plain Rectangle (not a
// FocusScope), so it is false exactly when a descendant text box holds focus —
// while this one is gated on the focus item exposing redo(), which the
// Rectangle does not. Only ever one of the two is enabled.
//
// Non-visual: it exists only to host the Shortcut and to reach the window via
// the Window attached property (a bare Shortcut is a QObject and has none).
//
// focusItem is `var`, not `Item`: `.redo` / the call below are dynamic lookups
// on a type the QML compiler can't know statically, and a declared `Item` type
// invites a qmllint/AOT complaint about an unknown member.
Item {
    id: root

    visible: false
    width: 0
    height: 0

    readonly property var focusItem: root.Window.window ? root.Window.window.activeFocusItem : null
    readonly property bool canRedoHere: root.focusItem !== null
                                        && typeof root.focusItem.redo === "function"

    Shortcut {
        sequence: "Ctrl+Y"
        enabled: root.canRedoHere
        // Not gated on canRedo: an empty text-box redo stack makes this a
        // harmless no-op, and keeping the enable-gate to a single stable
        // predicate avoids a second binding that has to re-evaluate on every
        // keystroke.
        onActivated: root.focusItem.redo()
    }
}
