// Pure JS text/derivation helpers, extracted verbatim from Sidebar.qml and
// MainPanel.qml. Same contract as CombineRuleRegex.js: every function here is
// stateless — it depends only on its arguments and this module's own
// constants, never on a QML id, a component property, or AppSettings — so it
// lives in a shared `.pragma library` module rather than inline on a component.
//
// Several of these functions USED to read AppSettings (or a derived component
// property) mid-body, which a `.pragma library` module cannot see. Those reads
// are now explicit parameters, lifted at the (QML) call site:
//   highlightMatch/transcriptSnippet  <- AppSettings.highlightColor
//   formatGutterTimestamp             <- AppSettings.timestampDecimalPlaces
//                                        AND mainPanel.gutterMaxT0
//   renameInitialText/renameBaseValue/optimisticDisplayName
//                                     <- AppSettings.useLabelsSidebar
//                                        AND AppSettings.hideExtensionsSidebar
// Passing them in is not just a mechanical port — it's what makes each of
// these independently testable and keeps the settings reads at ONE visible
// place per call site instead of buried in a string builder.
//
// `Qt.hsla()` (combineGroupColor) and `qsTr()` (formatSilenceMarkerText) were
// both verified to work from inside a `.pragma library` module on this Qt
// build before being moved here — a shared script is stateless, but it is not
// cut off from the QML global object.
.pragma library

// --- Filename extension rule (single source) -----------------------------
//
// The "a file's extension is the text from its LAST dot, and only when that
// dot isn't the first character" rule was written out inline four separate
// times across the rename/display path (renameInitialText, renameBaseValue,
// optimisticDisplayName, and each row's own display-name derivation) — four
// chances for the dotfile guard (`dot > 0`, so ".hidden" keeps its whole name)
// to be fixed in one place and missed in the others. It lives here once now,
// as the two halves every one of those call sites actually wanted.

// "talk.m4a" -> "talk"; "talk" -> "talk"; ".hidden" -> ".hidden".
function stripExtension(name) {
    const dot = name.lastIndexOf(".")
    return dot > 0 ? name.substring(0, dot) : name
}

// The complement of stripExtension(): "talk.m4a" -> ".m4a"; "talk" -> "";
// ".hidden" -> "". Includes the dot, so stripExtension(n) + extensionOf(n)
// always reconstructs n exactly.
function extensionOf(name) {
    const dot = name.lastIndexOf(".")
    return dot > 0 ? name.substring(dot) : ""
}

// The final path segment of `path` ("/a/b/talk.m4a" -> "talk.m4a").
function baseNameOf(path) {
    return path.substring(path.lastIndexOf("/") + 1)
}

// --- Match highlighting + transcript snippet helpers ---------------------
// Rich-text (StyledText) escaping for plain segments around a highlight;
// Qt rich text supports inline background-color spans fine (only
// border-radius is unsupported).
function escapeHtml(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
}

// Wraps the first case-insensitive occurrence of query in text with a
// highlight span; returns escaped-but-unwrapped text if there's no match.
// `highlightColor` is AppSettings.highlightColor, lifted to a parameter (this
// module can't reach AppSettings) — it is interpolated into an HTML style
// attribute, exactly as before.
function highlightMatch(text, query, highlightColor) {
    const q = String(query)
    const t = String(text)
    if (q === "")
        return escapeHtml(t)
    const idx = t.toLowerCase().indexOf(q.toLowerCase())
    if (idx === -1)
        return escapeHtml(t)
    const before = escapeHtml(t.substring(0, idx))
    const match = escapeHtml(t.substring(idx, idx + q.length))
    const after = escapeHtml(t.substring(idx + q.length))
    return before + "<span style=\"background-color:" + highlightColor + "\">" + match + "</span>" + after
}

// Collapses a raw transcript body (with "[start - end] text [SPEAKER_TURN]"
// segment lines) into a single normalized-whitespace string for snippet
// display — the sidebar has no need for segment boundaries, unlike
// MainPanel's cross-segment search.
function cleanTranscript(body) {
    return String(body).split("\n").map(function (line) {
        return line.replace(/^\[\d{2}:\d{2}:\d{2}\.\d{3} - \d{2}:\d{2}:\d{2}\.\d{3}\]\s*/, "")
                    .replace(/\s*\[SPEAKER_TURN\]\s*$/, "")
    }).join(" ").replace(/\s+/g, " ").trim()
}

// Ellipsis-bounded, highlighted snippet of cleaned transcript text around
// the first match. Returns "" if query doesn't occur (caller should not
// show a snippet row in that case). `highlightColor` is lifted from
// AppSettings.highlightColor — see highlightMatch() above.
function transcriptSnippet(cleanedBody, query, contextChars, highlightColor) {
    const q = String(query)
    const body = String(cleanedBody)
    if (q === "" || body === "")
        return ""
    const idx = body.toLowerCase().indexOf(q.toLowerCase())
    if (idx === -1)
        return ""
    const start = Math.max(0, idx - contextChars)
    const end = Math.min(body.length, idx + q.length + contextChars)
    const before = (start > 0 ? "… " : "") + escapeHtml(body.substring(start, idx))
    const match = escapeHtml(body.substring(idx, idx + q.length))
    const after = escapeHtml(body.substring(idx + q.length, end)) + (end < body.length ? " …" : "")
    return before + "<span style=\"background-color:" + highlightColor + ";color:white;font-weight:bold\">" + match + "</span>" + after
}

// Stable, distinct color per group, derived from the
// group's outputPath (unique per group, stable across rescans) via a
// simple string hash -> hue — no sequential group-index bookkeeping
// needed, so it stays correct even as groups appear/disappear/reorder.
// Fixed saturation/lightness chosen to stay readable against this app's
// dark sidebar background regardless of hue.
function combineGroupColor(outputPath) {
    let hash = 0
    for (let i = 0; i < outputPath.length; i++)
        hash = (hash * 31 + outputPath.charCodeAt(i)) | 0
    const hue = (Math.abs(hash) % 360) / 360
    return Qt.hsla(hue, 0.55, 0.6, 1.0)
}

// Is `path` the folder `folderPath` itself, or anything inside it? The
// vault tree IS the directory structure, so containment is a pure path
// prefix test — no model walk needed. The trailing "/" matters: without
// it "Interviews2/x.mp3" would count as being inside "Interviews".
function isUnderFolder(path, folderPath) {
    return folderPath !== "" && path !== ""
            && (path === folderPath || path.startsWith(folderPath + "/"))
}

// --- Sidebar inline-rename text derivation ------------------------------
//
// All three take the two sidebar display toggles as explicit booleans
// (`useLabels` = AppSettings.useLabelsSidebar, `hideExtensions` =
// AppSettings.hideExtensionsSidebar) rather than reading them, so the rule
// they encode is one testable function of its inputs.

// The display name a row WILL show after a rename to newBase — mirrors
// rowNameText's own base derivation so the optimistic frame matches the
// rebuilt one exactly. A label edit shows the label verbatim; a filename
// edit shows base+extension when extensions are visible (renameFile
// re-appends the real extension), base alone when they're hidden.
function optimisticDisplayName(path, isDir, label, newBase, useLabels, hideExtensions) {
    if (!isDir && useLabels && label)
        return newBase
    if (isDir || !hideExtensions) {
        if (isDir)
            return newBase
        return newBase + extensionOf(baseNameOf(path))
    }
    return newBase
}

// The text the editor opens with — always what renameFile()/renameFolder()
// actually consume, which is NOT always what the row displays:
//
// - A folder edits its own name.
// - A note showing a stored label (AppSettings.useLabelsSidebar) edits that
//   LABEL, not the filename — you rename what you can see, matching
//   MainPanel's titleField convention for the same toggle.
// - Any other note edits its BASE name, extension always stripped, even
//   when AppSettings.hideExtensionsSidebar is off and the row therefore
//   shows "talk.m4a". renameFile() takes a base name and re-appends the
//   real extension itself, so seeding "talk.m4a" here would rename the file
//   to "talk.m4a.m4a". The editor showing one fewer token than the row did
//   is the honest representation of what's being edited.
function renameInitialText(path, isDir, label, useLabels, hideExtensions) {
    const base = baseNameOf(path)
    if (isDir)
        return base
    if (useLabels && label)
        return label
    // A filename edit seeds the SAME text the row shows (rowNameText): the
    // full base+extension when extensions are visible, the stem alone when
    // hidden. renameFile() takes a stem and re-appends the real extension
    // itself, so renameBaseValue() strips the extension back off on commit
    // (mirrors MainPanel's filenameSubtitle / commitFilenameRename).
    if (!hideExtensions)
        return base
    return stripExtension(base)
}

// The value the write actually consumes, derived from the editor's display
// text: a folder or a label edits its full text, but a filename edit shows
// the extension (when visible) that renameFile() must NOT receive — strip
// it back to the stem. Any extension the user typed is ignored; the file
// keeps its real one (same contract as MainPanel's commitFilenameRename).
function renameBaseValue(path, isDir, label, text, useLabels, hideExtensions) {
    if (isDir || (useLabels && label) || hideExtensions)
        return text
    return stripExtension(text)
}

// --- MainPanel transcript formatting ------------------------------------

// A speaker's default, never-renamed display name is always exactly
// "Speaker N" (see DiarizationEngine/labelForSegment) — used to decide
// which side of a merge "wins" the resulting name.
function isDefaultSpeakerName(name) {
    return /^Speaker \d+$/.test(name)
}

// Compact gutter timestamp: only the start time (no range), zero-padded to
// fit the file's LARGEST timestamp (`maxT0`, the caller's mainPanel.gutterMaxT0)
// so the column reads as an aligned ruler — the leading unit is widened to the
// digit-count of that unit in the max, lower units always two digits. A file
// that runs into hours renders every row as H..:MM:SS (a 3s row -> "0:00:03",
// or "00:00:03" past 9h); one that only reaches minutes as M..:SS ("00:03" /
// "0:03"); a sub-minute file as bare seconds. Fractional-second precision is
// governed by `decimals` (the caller's AppSettings.timestampDecimalPlaces, 0 by
// default — display-only; the stored data always keeps full precision, and
// trims BOTH leading and trailing zeros, see formatTimestamp() in
// TranscriptionController.cpp — do not conflate the two).
//
// BOTH `maxT0` and `decimals` were AppSettings/component reads inside this
// body before it moved here. `maxT0` in particular was read ONLY in the
// padding-tier block below, well after the first use of `decimals` — easy to
// miss when scanning the top of the function for hidden dependencies.
function formatGutterTimestamp(t0, maxT0, decimals) {
    const scale = Math.pow(10, decimals)
    const totalUnits = Math.round(t0 * scale)
    const wholeSeconds = Math.floor(totalUnits / scale)
    const frac = totalUnits % scale
    const fracPart = decimals > 0 ? "." + String(frac).padStart(decimals, "0") : ""
    const h = Math.floor(wholeSeconds / 3600)
    const m = Math.floor((wholeSeconds % 3600) / 60)
    const s = wholeSeconds % 60

    // Padding tier + leading-unit width come from the file's max start.
    const maxWhole = Math.floor(maxT0)
    const maxH = Math.floor(maxWhole / 3600)
    const maxM = Math.floor((maxWhole % 3600) / 60)
    if (maxH > 0) {
        const hStr = String(h).padStart(String(maxH).length, "0")
        return hStr + ":" + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0") + fracPart
    }
    if (maxM > 0) {
        const mStr = String(m).padStart(String(maxM).length, "0")
        return mStr + ":" + String(s).padStart(2, "0") + fracPart
    }
    return String(s).padStart(String(maxWhole).length, "0") + fracPart
}

// StyleGuide.md "Transcript silence divider": U+00B7 MIDDLE DOT x3 either
// side of a compact duration label, e.g. "··· 34s silence ···". One
// function keeps the glyphs/spacing/wording in a SINGLE place rather
// than string-building them ad hoc at the (one) call site.
function formatSilenceMarkerText(seconds) {
    const rounded = Math.max(1, Math.round(seconds))
    const minutes = Math.floor(rounded / 60)
    const secs = rounded % 60
    const durationLabel = minutes > 0
        ? (secs > 0 ? qsTr("%1m %2s").arg(minutes).arg(secs) : qsTr("%1m").arg(minutes))
        : qsTr("%1s").arg(secs)
    return qsTr("··· %1 silence ···").arg(durationLabel)
}
