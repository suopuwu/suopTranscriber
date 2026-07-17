pragma Singleton
import QtQuick

// The app's ONE floating-notification queue (see StyleGuide.md "Snackbars").
// Any component posts here — Notifications.post({...}) — and SnackbarHost
// (instantiated once, in Main.qml) renders the queue bottom-right. This
// replaces the four hand-rolled auto-clearing overlay banners that each
// component used to carry its own copy of (importError, rowActionError,
// speakerOpError, transcriptEditError); a singleton is what lets Sidebar and
// MainPanel post without threading signals up to Main.qml, and matches the
// AppSettings singleton idiom already in the tree.
//
// A singleton rather than per-panel state is also the point of the feature:
// every transient notification lands in ONE place, so the user learns to look
// at one corner instead of four.
//
// NOT for a message that explains the state of a specific control you're
// looking at (an invalid regex by the search bar, a bad folder by its picker)
// — those stay inline Labels per StyleGuide.md "Warning banners". A field's
// error belongs beside the field, not in the corner of the screen.
QtObject {
    id: root

    // ListModel, NOT a `property var` JS array, and this is load-bearing:
    // dismissing/expiring one entry must not disturb the others' delegates.
    // Reassigning a whole array rebuilds every delegate in the Repeater, which
    // destroys a snackbar out from under an in-flight click on ANOTHER
    // snackbar's action button — entry #1's 5s timer firing while you're
    // pressing entry #2's "Undo" would eat the click. ListModel.remove()
    // touches exactly the one delegate.
    //
    // Rows hold display data only (text/kind/token/actionLabels). Action
    // CALLBACKS live in _callbacks below, keyed by token, so nothing has to
    // stuff a JS function into a model role (which would force
    // dynamicRoles: true and its whole-model coercion caveats).
    readonly property ListModel queue: ListModel {}

    // token -> { actions: [{label, onTriggered}], onDismissed }
    property var _callbacks: ({})
    property int _nextToken: 1

    readonly property int defaultTimeout: 6000

    // Bound by SnackbarHost while the pointer is over the stack: an entry must
    // not expire out from under a user who is reaching for its Undo button.
    // Timers live on the delegates; this just gates them.
    property bool paused: false

    signal posted(int token)
    signal removed(int token)

    // cfg: {
    //   text:        string, required — the message
    //   kind:        "info" | "warning" | "error"  (default "info")
    //   actions:     [{ label: string, onTriggered: function }]  (default [])
    //   timeout:     ms before auto-dismiss; 0 = sticky until dismissed
    //                (default defaultTimeout; an entry WITH actions still
    //                auto-expires — the X is for getting rid of it early, not
    //                the only way out)
    //   onDismissed: function, called when the entry leaves for any reason
    // }
    // Returns the entry's token (pass to dismiss()).
    function post(cfg) {
        const token = root._nextToken++
        const actions = cfg.actions || []

        root._callbacks[token] = {
            actions: actions,
            onDismissed: cfg.onDismissed || null
        }

        // Nested array-of-objects -> ListModel builds a child ListModel for
        // the `actions` role, which the delegate's Repeater consumes directly.
        root.queue.append({
            token: token,
            text: cfg.text,
            kind: cfg.kind || "info",
            timeout: cfg.timeout === undefined ? root.defaultTimeout : cfg.timeout,
            actionLabels: actions.map(a => ({ label: a.label }))
        })
        root.posted(token)
        return token
    }

    // Convenience wrappers — the overwhelmingly common shapes.
    function error(text, actions) {
        return root.post({ text: text, kind: "error", actions: actions || [] })
    }
    function warn(text, actions) {
        return root.post({ text: text, kind: "warning", actions: actions || [] })
    }
    function info(text, actions) {
        return root.post({ text: text, kind: "info", actions: actions || [] })
    }

    function _indexOf(token) {
        for (let i = 0; i < root.queue.count; ++i)
            if (root.queue.get(i).token === token)
                return i
        return -1
    }

    // Fire action `index` of `token`, then dismiss the entry — an action is
    // always terminal (you undo once; a second Undo would re-trash). The
    // callback runs BEFORE the row leaves the model so the delegate that's
    // mid-click is still alive while its own handler runs.
    function trigger(token, index) {
        const entry = root._callbacks[token]
        if (!entry || index < 0 || index >= entry.actions.length)
            return
        const action = entry.actions[index]
        if (action.onTriggered)
            action.onTriggered()
        root.dismiss(token)
    }

    function dismiss(token) {
        const i = root._indexOf(token)
        if (i < 0)
            return
        const entry = root._callbacks[token]
        root.queue.remove(i)
        delete root._callbacks[token]
        if (entry && entry.onDismissed)
            entry.onDismissed()
        root.removed(token)
    }

    function clear() {
        while (root.queue.count > 0)
            root.dismiss(root.queue.get(0).token)
    }
}
