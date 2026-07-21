import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// One chapter-heading row per chapter marker interleaved into the
// transcript (chapterHeadingRepeater's delegate). Disclosure triangle +
// click-to-edit inline title. Extracted verbatim from MainPanel; see the
// threaded `panel`/`txArea` properties below.
RowLayout {
    // Threaded from the instantiation site so the panel.* and
    // txArea.* references in this delegate resolve exactly as they
    // did inline (editingChapterTs, collapsedChapters, commitChapterTitle,
    // toggleChapterCollapse, selectionSweepsHeading; gutterLineRects,
    // liveStartOf). modelData is filled by the Repeater.
    property var panel: null
    property var txArea: null

    id: chapterHeadingRow
    objectName: "chapterHeadingRow"
    required property var modelData
    readonly property bool collapsed:
        !!panel.collapsedChapters[String(chapterHeadingRow.modelData.chapterT0)]
    readonly property rect lineRect:
        txArea.gutterLineRects[chapterHeadingRow.modelData.idx] || Qt.rect(0, 0, 0, 0)
    x: 0
    y: lineRect.y
    width: txArea.width
    height: lineRect.height
    spacing: 8

    // Opens the title for editing from an at-rest tap, with
    // the caret at the click (clickX is in headingTitle's
    // coordinates). The press can't place it itself —
    // selectByMouse is false until this title IS the active
    // edit — and selecting the whole title instead would
    // make the next keystroke wipe it.
    //
    // ONLY the at-rest tap routes through here. Once
    // editing, the field's own selectByMouse places the
    // caret and drives drag-select natively; stepping in
    // would clobber both. That's why headingTitle has to
    // span the row (see its Layout note) rather than sit
    // beside a tap-target strip: a strip can begin an edit,
    // but it can't extend a selection.
    function beginTitleEdit(clickX) {
        if (headingTitle.editing)
            return
        panel.editingChapterTs = chapterHeadingRow.modelData.chapterT0
        headingTitle.forceActiveFocus()
        headingTitle.cursorPosition =
            headingTitle.positionAt(clickX, headingTitle.height / 2)
    }

    // Item 33: a dedicated disclosure triangle collapses/
    // expands this chapter's body in the transcript
    // (displayData drops the collapsed sentences). Kept
    // SEPARATE from the title (which is click-to-edit, item
    // 35) so the two gestures never collide — a triangle is
    // the least ambiguous collapse affordance (task note).
    AppIcon {
        Layout.alignment: Qt.AlignVCenter
        name: chapterHeadingRow.collapsed ? "chapter-expand" : "chapter-collapse"
        iconSize: 10
        color: "#8fdf9f"

        HoverHandler { cursorShape: Qt.PointingHandCursor }
        // ReleaseWithinBounds (not the default
        // DragThreshold) on every tap target in this row:
        // the row is an OVERLAY sitting on top of
        // txArea, and DragThreshold takes only a
        // PASSIVE grab — the press falls straight through
        // to the TextArea underneath, which focuses itself
        // and drops a cursor on the heading's blank
        // reserved line. An exclusive grab from press is
        // what keeps a click on this row from also being a
        // click in the transcript body.
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: panel.toggleChapterCollapse(chapterHeadingRow.modelData.chapterT0)
        }
    }
    // Item 35: the transcript heading is now click-to-edit
    // too, sharing the SAME single-active-edit state
    // (editingChapterTs, keyed by the real chapterT0) as
    // the TOC row, so editing one exits the other. Commits
    // through setChapterTitle via commitChapterTitle().
    TextEdit {
        id: headingTitle
        objectName: "chapterHeadingTitle"
        readonly property bool editing:
            panel.editingChapterTs === chapterHeadingRow.modelData.chapterT0
        // Spans the whole row right of the triangle, not
        // just its text: everything to the right of the
        // title still LOOKS like the heading line, and a
        // click or drag out there has to be a real click in
        // THIS field (caret to the end of the title, native
        // drag-select back through it). A sibling strip
        // holding the rule can't do that — its tap grabs the
        // press, so a click past the last character while
        // editing did nothing and a drag from out there
        // selected nothing. Filling the width also keeps the
        // press off txArea underneath, which is what
        // the strip was there for.
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignVCenter
        readOnly: !headingTitle.editing
        selectByMouse: headingTitle.editing
        selectByKeyboard: headingTitle.editing
        activeFocusOnPress: false
        text: chapterHeadingRow.modelData.chapterTitle
        color: "#8fdf9f"
        font.pixelSize: 12
        font.bold: true
        // The SAME selection colour txArea uses (see
        // its selectionColor) — a title swept by a transcript
        // selection (sweptBySelection below) has to read as
        // part of that one selection, not a separate widget
        // lighting up in the Basic palette's opaque navy.
        selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)

        // A drag through the transcript can't natively
        // select this title (it's a separate document — see
        // selectionSweepsHeading), so when the transcript's
        // selection sweeps across this heading's line, mirror
        // it here: the title highlights along with the body
        // around it, and copySelectionWithChapters() splices
        // the same titles into the clipboard, keyed off the
        // same predicate so what you copy is what you saw
        // highlighted. All-or-nothing per title — the line it
        // reserves is zero-length, so there's no sub-title
        // offset a transcript selection could even land on.
        //
        // Never while editing: this title's own selection is
        // the user's then, and stomping it would undo their
        // drag-select mid-gesture.
        readonly property bool sweptBySelection:
            !headingTitle.editing
            && panel.selectionSweepsHeading(
                txArea.liveStartOf(chapterHeadingRow.modelData))
        onSweptBySelectionChanged: {
            if (headingTitle.sweptBySelection)
                headingTitle.select(0, headingTitle.length)
            else
                headingTitle.deselect()
        }

        HoverHandler { cursorShape: Qt.IBeamCursor }
        // Only the tapped instance focuses itself (see the
        // TOC title's twin handler for why).
        // Default (DragThreshold/passive) gesturePolicy
        // here, unlike this row's other tap targets: a
        // TextEdit accepts the press ITSELF, so nothing
        // falls through to txArea anyway, and an
        // exclusive grab would swallow the presses that
        // place the cursor / drag-select once this title
        // IS being edited.
        TapHandler {
            onTapped: (eventPoint) => chapterHeadingRow.beginTitleEdit(eventPoint.position.x)
        }
        onEditingChanged: {
            if (!headingTitle.editing && headingTitle.activeFocus)
                headingTitle.focus = false
        }
        onActiveFocusChanged: {
            if (!headingTitle.activeFocus) {
                panel.commitChapterTitle(chapterHeadingRow.modelData.chapterT0,
                                             chapterHeadingRow.modelData.chapterTitle,
                                             headingTitle.text)
                if (panel.editingChapterTs === chapterHeadingRow.modelData.chapterT0)
                    panel.editingChapterTs = -1
            }
        }
        Keys.onReturnPressed: (event) => {
            event.accepted = true
            panel.editingChapterTs = -1
        }
        Keys.onEnterPressed: (event) => {
            event.accepted = true
            panel.editingChapterTs = -1
        }
        Keys.onEscapePressed: (event) => {
            event.accepted = true
            headingTitle.text = chapterHeadingRow.modelData.chapterTitle
            panel.editingChapterTs = -1
        }

        // The rule filling the heading line to the right of
        // the title. A CHILD of the field, offset by
        // contentWidth (the text's real width), rather than
        // a sibling laid out beside it — the field has to
        // own that space for a click/drag past the last
        // character to reach the text, so the rule has to
        // live inside it. It's a bare Rectangle: it accepts
        // no mouse of its own, and presses land on the
        // TextEdit behind it, which is exactly what we want.
        Rectangle {
            x: headingTitle.contentWidth + 8
            width: Math.max(0, headingTitle.width - x)
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: Theme.border
        }
    }
}
