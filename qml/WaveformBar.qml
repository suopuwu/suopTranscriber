import QtQuick
import SuopTranscriber

// Reusable time-windowed waveform + playhead. Extracted out of
// RecorderPage.qml's inline Canvas so a second call site
// can reuse the same display without dragging the recorder along with it —
// this file must stay ignorant of RecorderPage/AudioRecorder/AudioPlayer;
// everything it needs comes in as a property, and a seek is reported through
// seekRequested rather than written into a player property directly.
//
// WHY WINDOWED, NOT WHOLE-TAKE: plotting the entire recording's samples across
// a fixed-width canvas crushes a long take into a solid mass with no readable
// detail (a five-second take reads fine, a ten-minute one is a green blob).
// Showing a fixed `windowMs` slice around the current position, instead of
// the whole span, keeps the pixels-per-second ratio constant regardless of
// how long the take is.
Item {
    id: root

    // {ms, level} pairs, oldest first, level in 0..1 (see
    // AudioRecorderIO::displayLevelFromRms). Not required to be evenly
    // spaced — the ms IS the x-axis.
    property var samples: []
    // Visible span of the waveform, in ms. 30s default.
    property int windowMs: 30000
    // Where the playhead is, and the take's full extent. Both in ms.
    property real positionMs: 0
    property real durationMs: 0
    // Live capture: the window pins to the newest sample and seeking is off.
    property bool live: false

    // Not "not yet metered" — a distinct rendering mode. -1 means draw the
    // normal windowed wave; anything else replaces it with one static bar at
    // this level, for when AppSettings.animationsEnabled is off (that flag
    // kills continuous-update CPU/GPU work app-wide, waveform
    // included). The caller decides when that applies; this component only
    // renders whichever mode it's handed.
    property real staticLevel: -1

    property color waveColor: "#6fdf9f"
    property color playheadColor: "#e07a7a"

    // "filled" (closed mirrored/baseline body) or "line" (stroked outline
    // only) — see the paint path below for the split.
    property string waveStyle: "filled"
    // Mirror the wave about the vertical centre. When false the baseline
    // moves to the bottom edge and the excursion becomes the full height
    // (rather than half, doubled) so a single-sided wave uses the same
    // vertical range as a mirrored one and doesn't look half as loud.
    property bool mirrored: false
    property real lineWidth: 1.5

    // Click/drag to seek. Off by default so a second call site that only
    // wants a display gets one; the recorder turns it on.
    property bool interactive: false
    signal seekRequested(real ms)

    // Pure, and the unit under test. durationMs <= windowMs means the whole
    // take fits without scrolling; otherwise the window centres on
    // positionMs, clamped so it never runs past [0, durationMs]. Written as a
    // plain function (not inlined into windowStartMs's binding) so a test can
    // call it directly without disturbing this item's own bindings.
    function windowStartFor(positionMs, durationMs, windowMs) {
        if (durationMs <= windowMs)
            return 0
        const start = positionMs - windowMs / 2
        return Math.max(0, Math.min(start, durationMs - windowMs))
    }
    readonly property real windowStartMs:
        root.windowStartFor(root.positionMs, root.durationMs, root.windowMs)

    // First index whose sample.ms >= ms, via binary search — samples is
    // ordered oldest-first, and a repaint must not walk every one of up to
    // 12000 entries just to find the ~width that fall inside the window.
    function lowerBoundMs(list, ms) {
        let lo = 0
        let hi = list.length
        while (lo < hi) {
            const mid = (lo + hi) >> 1
            if (list[mid].ms < ms)
                lo = mid + 1
            else
                hi = mid
        }
        return lo
    }

    // ONE axis, the whole take, at any height — the bar used to seek by
    // window above a bottom strip and by whole-take on it, but that strip is
    // gone and an invisible band that answers a click differently reads as a
    // bug. Clamped into [0, durationMs] so a drag past the item's bounds
    // can't request a negative or out-of-range seek.
    //
    // Accepted cost: past windowMs the wave is a scrolling slice while x maps
    // to the whole file, so clicking a visible peak on a long take does not
    // land on that peak. Below windowMs — the recorder's normal case — window
    // and take are the same span, so the two axes coincide exactly.
    function seekTo(x) {
        if (!root.interactive || root.live || root.durationMs <= 0)
            return
        root.seekRequested(Math.max(0, Math.min(root.durationMs, root.durationMs * x / root.width)))
    }

    TapHandler {
        id: seekTap
        enabled: root.interactive && !root.live && root.durationMs > 0
        onTapped: root.seekTo(seekTap.point.position.x)
    }
    // target: null — the bar itself never moves; this just reports where the
    // pointer is so drags scrub the same as taps. No MouseArea: it always
    // claims the cursor even when inert, and swallows clicks under a
    // ScrollView (see MiddleDragScroll.qml's note) — Item + handlers only.
    DragHandler {
        id: seekDrag
        target: null
        enabled: root.interactive && !root.live && root.durationMs > 0
        onCentroidChanged: if (seekDrag.active)
                                root.seekTo(seekDrag.centroid.position.x)
    }
    HoverHandler {
        cursorShape: Qt.PointingHandCursor
        enabled: root.interactive
    }

    Canvas {
        id: canvas
        anchors.fill: parent

        onWidthChanged: canvas.requestPaint()
        onHeightChanged: canvas.requestPaint()
        // The bar is typically built eagerly while its page is hidden (see
        // RecorderPage.qml), so `visible` here is EFFECTIVE visibility —
        // without this the empty frame's first paint depends on a size
        // change landing at a renderable moment.
        onVisibleChanged: if (canvas.visible) canvas.requestPaint()

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()

            // Frame FIRST, wave second — every path below can return early
            // (no samples yet, static mode), and the empty frame must still
            // be the thing left on screen in those states. Inset by half a
            // pixel so the 1px stroke lands on whole pixels instead of
            // straddling the canvas edge.
            const frameRadius = AppSettings.borderRadius
            ctx.beginPath()
            ctx.roundedRect(0.5, 0.5, width - 1, height - 1, frameRadius, frameRadius)
            // Darker than the page behind it so the display reads as an
            // inset screen — but it is the outline, not this fill, that
            // makes it visible at rest.
            ctx.fillStyle = "#1a1a1a"
            ctx.fill()
            ctx.lineWidth = 1
            // The same dim outline an unselected, unhovered toggle wears — a
            // full-white frame would read as an active element rather than a
            // container.
            ctx.strokeStyle = Theme.borderStrong
            ctx.stroke()

            // Clip strictly INSIDE the stroke: a full-scale wave spans the
            // whole height at the left/right edges and would otherwise paint
            // over the outline and out through the rounded corners.
            const innerRadius = Math.max(0, frameRadius - 1)
            ctx.beginPath()
            ctx.roundedRect(1, 1, width - 2, height - 2, innerRadius, innerRadius)
            ctx.clip()

            // Baseline and excursion for one sample's y — see the `mirrored`
            // doc comment above. Mirrored: baseline at centre, half-height
            // excursion doubled by the bottom edge. Unmirrored: baseline at
            // the bottom edge, full-height excursion in one direction only.
            const mid = root.mirrored ? height / 2 : height - 1
            const range = root.mirrored ? mid : height

            if (root.staticLevel >= 0) {
                const level = root.staticLevel
                const topY = mid - level * range
                if (root.waveStyle === "line") {
                    ctx.beginPath()
                    ctx.moveTo(0, topY)
                    ctx.lineTo(width, topY)
                    if (root.mirrored) {
                        const bottomY = mid + level * range
                        ctx.moveTo(0, bottomY)
                        ctx.lineTo(width, bottomY)
                    }
                    ctx.strokeStyle = root.waveColor
                    ctx.lineWidth = root.lineWidth
                    ctx.stroke()
                } else {
                    const bottomY = root.mirrored ? mid + level * range : mid
                    ctx.fillStyle = root.waveColor
                    ctx.fillRect(0, topY, width, bottomY - topY)
                }
                return
            }

            const windowEnd = root.windowStartMs + root.windowMs
            const samples = root.samples
            const start = root.lowerBoundMs(samples, root.windowStartMs)
            let end = start
            while (end < samples.length && samples[end].ms <= windowEnd)
                end++

            if (end > start) {
                const xFor = i => width * (samples[i].ms - root.windowStartMs) / root.windowMs
                const topYFor = i => mid - samples[i].level * range

                if (root.waveStyle === "line") {
                    ctx.beginPath()
                    for (let i = start; i < end; i++) {
                        if (i === start)
                            ctx.moveTo(xFor(i), topYFor(i))
                        else
                            ctx.lineTo(xFor(i), topYFor(i))
                    }
                    ctx.strokeStyle = root.waveColor
                    ctx.lineWidth = root.lineWidth
                    ctx.stroke()

                    // Stroked as its own path, not joined onto the top
                    // polyline — closing the two into one path would draw a
                    // vertical segment at each window end connecting them.
                    if (root.mirrored) {
                        ctx.beginPath()
                        for (let i = start; i < end; i++) {
                            const y = mid + samples[i].level * range
                            if (i === start)
                                ctx.moveTo(xFor(i), y)
                            else
                                ctx.lineTo(xFor(i), y)
                        }
                        ctx.stroke()
                    }
                } else {
                    ctx.beginPath()
                    for (let i = start; i < end; i++) {
                        if (i === start)
                            ctx.moveTo(xFor(i), topYFor(i))
                        else
                            ctx.lineTo(xFor(i), topYFor(i))
                    }
                    // Bottom edge: mirrored walks each sample's own mirrored
                    // level back; unmirrored just closes along the baseline,
                    // filling the shape down to it like a graph to its axis.
                    for (let i = end - 1; i >= start; i--) {
                        const y = root.mirrored ? mid + samples[i].level * range : mid
                        ctx.lineTo(xFor(i), y)
                    }
                    ctx.closePath()
                    ctx.fillStyle = root.waveColor
                    ctx.fill()
                }
            }

            // Where the audio ends. Only below windowMs, because that is the
            // only case it says anything: the window is pinned at 0 and the
            // wave fills just durationMs/windowMs of the frame, so without
            // this the empty remainder reads as recorded silence rather than
            // as past the end. At or above windowMs the frame is full and the
            // line would sit on the right edge under the outline. Border
            // colour, not the wave's: it is frame furniture marking the edge
            // of the data, not a reading taken from it.
            if (root.durationMs > 0 && root.durationMs < root.windowMs) {
                ctx.fillStyle = Theme.borderStrong
                ctx.fillRect(width * root.durationMs / root.windowMs, 0, 1, height)
            }

            // LAST — the playhead is the only progress indicator now (the
            // full-take progress strip that used to run along the bottom is
            // gone), so nothing may paint over it.
            if (root.durationMs > 0) {
                const playX = Math.max(0, Math.min(width,
                    width * (root.positionMs - root.windowStartMs) / root.windowMs))
                ctx.fillStyle = root.playheadColor
                ctx.fillRect(playX, 0, 2, height)
            }
        }
    }

    onSamplesChanged: canvas.requestPaint()
    onPositionMsChanged: canvas.requestPaint()
    onDurationMsChanged: canvas.requestPaint()
    onWindowMsChanged: canvas.requestPaint()
    onLiveChanged: canvas.requestPaint()
    onStaticLevelChanged: canvas.requestPaint()
    onWaveStyleChanged: canvas.requestPaint()
    onMirroredChanged: canvas.requestPaint()
    onLineWidthChanged: canvas.requestPaint()
}
