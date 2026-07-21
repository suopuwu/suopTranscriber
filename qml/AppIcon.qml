import QtQuick
import SuopTranscriber

// The single way this app draws an icon. Call sites name an icon
// semantically ("transcribe", "delete-file") and never mention a glyph, a
// file, or a pack — which pack supplies it is IconTheme's decision, driven by
// AppSettings.iconPack plus any per-icon AppSettings.iconOverrides entry.
//
//     AppIcon { name: "transcribe"; iconSize: 15; color: "#dddddd" }
//
// `iconSize` is the icon's BOX, not a font size. Per-glyph optical
// correction belongs in the pack manifest's per-icon `scale`, not here: the
// same semantic icon used to be written as font.pixelSize 13 in one cluster
// and 15 in another for no reason anyone recorded, which is exactly the
// inconsistency this component exists to end. Pass the cluster's canonical
// size and let the pack correct individual glyphs.
Item {
    id: root

    // A semantic name from the pack manifests — see IconTheme::iconNames().
    property string name: ""
    // The box the icon occupies, in logical px.
    property real iconSize: 15
    property color color: Theme.textPrimary
    // Force a specific pack, ignoring the user's settings — for previewing a
    // pack the user hasn't selected (e.g. the Settings pack-picker chips).
    // Empty (the default) means "follow AppSettings", the normal case.
    property string packOverride: ""

    // Pure-function resolution: reading AppSettings.iconPack and
    // AppSettings.iconOverrides HERE is what registers them as binding
    // dependencies, so switching packs (or overriding one icon) re-resolves
    // every AppIcon in the app with no change signals to wire up.
    readonly property var resolved: root.packOverride !== ""
        ? IconTheme.resolve(root.name, root.packOverride, ({}))
        : IconTheme.resolve(root.name, AppSettings.iconPack, AppSettings.iconOverrides)
    readonly property string kind: root.resolved.kind || ""
    // Pack-supplied optical correction (see the manifests' per-icon `scale`).
    readonly property real appliedSize: root.iconSize * (root.resolved.scale || 1.0)

    implicitWidth: root.iconSize
    implicitHeight: root.iconSize

    // Deliberately not warning until Component.onCompleted: `resolved`
    // evaluates once during creation, before the AppSettings singleton's own
    // bindings have settled, and that first pass legitimately resolves to
    // nothing. Warning on it would fire spuriously for every icon in the app
    // on every startup, which is exactly enough noise to make a real
    // "nothing supplies this name" report invisible.
    property bool _resolutionSettled: false
    Component.onCompleted: {
        root._resolutionSettled = true
        root._warnIfUnresolved()
    }
    onResolvedChanged: if (root._resolutionSettled) root._warnIfUnresolved()

    // Same reasoning as the startup case above, for the OTHER transient: a
    // pooled view delegate being REUSED (TableView/TreeView recycle delegates
    // by default — see Sidebar.qml's tree) reassigns its required properties
    // one at a time, so `name` can already hold the incoming row's value while
    // `resolved`/`kind` still reflect the outgoing one. That momentary
    // disagreement is not a missing icon and settles within the frame; the
    // icon itself always drew correctly. It made every scroll/vault-change in
    // a tree big enough to recycle log "no icon pack supplies 'collapse'"
    // (user report) — confirmed by reuseItems:false making it vanish.
    //
    // So re-check on the next event-loop turn and only report a name that is
    // STILL unresolved once the bindings have settled. A genuinely absent icon
    // stays unresolved and is still reported, which is the whole point of the
    // warning — this only drops the transients.
    function _warnIfUnresolved() {
        if (root.name === "" || root.kind !== "")
            return
        Qt.callLater(root._warnIfStillUnresolved)
    }
    function _warnIfStillUnresolved() {
        if (root.name !== "" && root.kind === "")
            console.warn("AppIcon: no icon pack supplies '" + root.name + "'")
    }

    Loader {
        anchors.centerIn: parent
        sourceComponent: root.kind === "svg" ? svgComponent
                       : root.kind === "glyph" ? glyphComponent
                       : root.kind === "builtin" ? root.builtins[root.resolved.value] || null
                       : null
    }

    // Closed set — a manifest naming a builtin this map doesn't list resolves
    // to null and draws nothing, rather than loading an arbitrary QML URL a
    // pack could name.
    readonly property var builtins: ({
        "PlayGlyph": playGlyphComponent,
        "PauseGlyph": pauseGlyphComponent
    })

    Component {
        id: glyphComponent
        Text {
            text: root.resolved.value
            color: root.color
            font.pixelSize: root.appliedSize
            // Glyph packs resolve against whatever system font supplies the
            // codepoint, and those glyphs are rarely centered in their em-box
            // (that asymmetry is why play/pause are builtins). Centering on
            // the painted extent rather than the layout box keeps a glyph
            // pack's icons visually centered in their button.
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    Component {
        id: svgComponent
        Image {
            width: root.appliedSize
            height: root.appliedSize
            // The provider rasters at device pixels and this scales back down
            // to the logical box, so icons stay crisp on hiDPI. '#' would
            // otherwise start a URL fragment, hence the encode.
            source: "image://" + "suopicons" + "/" + root.resolved.pack + "/" + root.resolved.value
                    + "?color=" + encodeURIComponent(String(root.color))
                    + "&size=" + Math.max(1, Math.round(root.appliedSize * Screen.devicePixelRatio))
            smooth: true
        }
    }

    Component {
        id: playGlyphComponent
        PlayGlyph {
            width: root.appliedSize
            height: root.appliedSize
            color: root.color
        }
    }

    Component {
        id: pauseGlyphComponent
        PauseGlyph {
            width: root.appliedSize
            height: root.appliedSize
            color: root.color
        }
    }
}
