import QtQuick
import QtQuick.Shapes

// Rounded-rect perimeter outline for a row that's a member of a
// "ready" combine group (see Sidebar.qml), sized/weighted to match
// HoverBorder exactly (same strokeWidth, same corner radius) — unlike
// HoverBorder though, each edge is independently suppressible via
// topRounded/bottomRounded, so a contiguous vertical run of same-group rows
// can share ONE unbroken perimeter instead of each row drawing its own
// closed box. A plain Rectangle can't do this: border.width always draws
// all 4 sides regardless of per-corner radius, so the shared seam between
// two merged rows would still show a horizontal line across it. Suppressing
// an edge here just sets that ShapePath's strokeWidth to 0 (drawing
// nothing) while the adjacent left/right edges run straight through to the
// row's true top/bottom instead of curving in — see topRounded/bottomRounded.
Shape {
    id: root
    property color strokeColor: "white"
    property real strokeWidth: 1
    property real cornerRadius: 0
    // False when this edge is a suppressed seam shared with a same-group
    // neighbor rather than a real (rounded) perimeter edge.
    property bool topRounded: true
    property bool bottomRounded: true
    // How far this Shape's own box is inset from the row's TRUE boundary
    // (i.e. whatever anchors.margins the caller applies) — must match that
    // margin exactly. A suppressed seam's overshoot (below) has to cross
    // this whole inset PLUS strokeWidth to actually reach the row boundary;
    // using strokeWidth alone here left a real gap once the caller's inset
    // grew past strokeWidth (regression: margins bumped 1->3 for outline
    // breathing room, but this was still only overshooting by 1.5, so
    // vertical edges of a merged run stopped connecting across rows).
    property real edgeMargin: 0

    preferredRendererType: Shape.CurveRenderer

    // Left edge: full height, stopping short by cornerRadius only at
    // whichever end has a real corner to curve into. Each merged seam is
    // bridged by exactly ONE of its two rows — the row above (whose BOTTOM
    // is suppressed) overshoots past its own boundary by strokeWidth +
    // edgeMargin; the row below (whose TOP is suppressed) stays flush at
    // 0/height and draws nothing extra. This asymmetry is deliberate: if
    // both sides overshot into the seam (the previous behavior), the same
    // pixels got painted by two independently-rendered anti-aliased Shape
    // instances, and that double coverage rendered as a visibly thicker
    // band right at every seam. A single overshooting side is enough to
    // bridge the gap (strokeWidth > edgeMargin for this component's caller)
    // without ever double-painting — refreshTreeOrdering() in Sidebar.qml
    // always pairs a row's bottomMerged with the row below's topMerged, so
    // the "giver" side is guaranteed to exist whenever the "receiver" side
    // does.
    ShapePath {
        strokeWidth: root.strokeWidth
        strokeColor: root.strokeColor
        fillColor: "transparent"
        capStyle: ShapePath.FlatCap
        startX: 0
        startY: root.topRounded ? root.cornerRadius : 0
        PathLine { x: 0; y: root.bottomRounded ? root.height - root.cornerRadius : root.height + root.strokeWidth + root.edgeMargin }
    }
    // Right edge: mirror of the left edge.
    ShapePath {
        strokeWidth: root.strokeWidth
        strokeColor: root.strokeColor
        fillColor: "transparent"
        capStyle: ShapePath.FlatCap
        startX: root.width
        startY: root.topRounded ? root.cornerRadius : 0
        PathLine { x: root.width; y: root.bottomRounded ? root.height - root.cornerRadius : root.height + root.strokeWidth + root.edgeMargin }
    }
    // Top edge + its two corner arcs — strokeWidth collapses to 0 (nothing
    // drawn) when this row's top is a suppressed seam, in which case the
    // left/right edges above already run flush to y:0 with no gap to fill.
    ShapePath {
        strokeWidth: root.topRounded ? root.strokeWidth : 0
        strokeColor: root.strokeColor
        fillColor: "transparent"
        capStyle: ShapePath.FlatCap
        startX: 0
        startY: root.cornerRadius
        PathArc { x: root.cornerRadius; y: 0; radiusX: root.cornerRadius; radiusY: root.cornerRadius }
        PathLine { x: root.width - root.cornerRadius; y: 0 }
        PathArc { x: root.width; y: root.cornerRadius; radiusX: root.cornerRadius; radiusY: root.cornerRadius }
    }
    // Bottom edge + its two corner arcs — mirror of the top edge.
    ShapePath {
        strokeWidth: root.bottomRounded ? root.strokeWidth : 0
        strokeColor: root.strokeColor
        fillColor: "transparent"
        capStyle: ShapePath.FlatCap
        startX: root.width
        startY: root.height - root.cornerRadius
        PathArc { x: root.width - root.cornerRadius; y: root.height; radiusX: root.cornerRadius; radiusY: root.cornerRadius }
        PathLine { x: root.cornerRadius; y: root.height }
        PathArc { x: 0; y: root.height - root.cornerRadius; radiusX: root.cornerRadius; radiusY: root.cornerRadius }
    }
}
