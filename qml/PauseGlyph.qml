import QtQuick

// The `classic` pack's "pause" icon (IconTheme resolves it as kind
// "builtin"). Counterpart to PlayGlyph — see its comment for why these two
// are drawn rather than font glyphs.
//
// The two bars are bbox-symmetric, so unlike PlayGlyph's triangle they need
// no centering offset: filling the full box centers them correctly.
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
        const barWidth = width * 0.3
        ctx.fillRect(0, 0, barWidth, height)
        ctx.fillRect(width - barWidth, 0, barWidth, height)
    }
}
