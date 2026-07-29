import QtQuick
import QtQuick.Controls
import SuopTranscriber

// A gain slider with the source's live level metered inside its own groove,
// the way a system volume slider shows what is actually playing through it.
//
// WHY IN THE GROOVE: the gain and the level it produces are one decision, and
// putting the meter anywhere else makes the user correlate two widgets while
// dragging. The fill is the instantaneous level and the line is the rolling
// peak (see AudioRecorderIO::decayPeak) — the fill tells you it is live, the
// line is what you actually match across sources.
//
// Colours come from the caller as already-resolved band colours rather than a
// threshold this file re-derives, so the meter, the numeric readout beside it,
// and any future meter all agree by construction — the band rule lives once,
// in AudioRecorderIO::levelBand().
Slider {
    id: control

    // Display levels, 0..1 (see AudioRecorderIO::displayLevelFromRms).
    property real level: 0
    property real peak: 0
    // Band colour for `level` / `peak`, resolved by the caller.
    property color levelColor: Theme.textMuted
    property color peakColor: Theme.textMuted
    // Meters go quiet when nothing is being captured, so a stale fill never
    // reads as a live signal.
    property bool metering: false

    hoverEnabled: true

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        implicitWidth: 200
        implicitHeight: 6
        width: control.availableWidth
        height: implicitHeight
        radius: height / 2
        color: Theme.surface
        border.width: 1
        border.color: Theme.borderStrong

        // The gain itself, filled from the left up to the handle — the same
        // read every other slider in the app gives (SliderSetting and SeekBar
        // both keep the Basic style's stock background, which draws this).
        // Distinct from levelFill below: that is the SIGNAL, and it reads 0
        // whenever nothing is being captured, which is exactly when the user
        // is setting gains by eye.
        Rectangle {
            id: gainFill
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: 1
            height: parent.height - 2
            width: control.visualPosition * (parent.width - 2)
            radius: height / 2
            color: control.enabled ? Theme.textMuted : Theme.borderStrong
            opacity: 0.7
        }

        // Live level. Kept UNDER the handle by being a child of the background,
        // so dragging never hides the thing being measured.
        Rectangle {
            id: levelFill
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: 1
            height: parent.height - 2
            width: control.metering
                   ? Math.max(0, Math.min(1, control.level)) * (parent.width - 2)
                   : 0
            radius: height / 2
            color: control.levelColor
            opacity: 0.55
            visible: width > 0
        }

        // Rolling peak — a line that stays where the maximum was, which is the
        // value you equalise against.
        Rectangle {
            id: peakLine
            width: 2
            height: parent.height + 4
            anchors.verticalCenter: parent.verticalCenter
            x: Math.max(0, Math.min(1, control.peak)) * (parent.width - width)
            color: control.peakColor
            visible: control.metering && control.peak > 0
        }
    }

    HoverBorder {
        anchors.fill: parent
        active: control.enabled && control.hovered
    }
}
