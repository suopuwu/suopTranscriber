import QtQuick

// The `classic` pack's "play" icon (IconTheme resolves it as kind "builtin").
//
// Drawn, not a font glyph: the "▶" Unicode character's em-box is never truly
// centered on its triangle (padding varies by font/system, so any fixed
// offset is a guess that breaks elsewhere) — a Canvas-drawn shape lets us
// control centering directly. The triangle's visual centroid (average of its
// 3 vertices) sits left of its bounding-box center, so it's drawn shifted
// right by a proportional offset to visually balance against PauseGlyph.
//
// The tip used to sit at width+xOffset, past the canvas's own
// raster — it got silently clipped, reading as a blunt/cut triangle. Capping
// the tip at `width` instead (the offset now only shifts the LEFT edge in,
// shrinking the triangle's span rather than pushing it past the bounds)
// keeps the whole shape on-canvas. The offset is a fraction of width rather
// than the original fixed 2px so it holds at any icon size, not just the
// 14x14 it was originally tuned at.
Canvas {
    id: glyph

    property color color: "white"

    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.fillStyle = glyph.color
        const xOffset = width * (2 / 14)
        ctx.beginPath()
        ctx.moveTo(xOffset, 0)
        ctx.lineTo(width, height / 2)
        ctx.lineTo(xOffset, height)
        ctx.closePath()
        ctx.fill()
    }
}
