// Pure JS helpers for the Combine-page regex/filename highlighter, extracted
// verbatim from SettingsPopup.qml. Every function here is stateless: it
// depends only on its arguments and this module's own constants — no QML ids,
// component properties, or AppSettings access — so it lives in a shared
// `.pragma library` module rather than inline on the popup. The hex colors in
// the `<span style="color:#...">` builders below are HTML string colors, NOT
// QML color bindings, and are intentionally left as literals.
.pragma library

// Not a full syntax highlighter — just enough to make regex
// metacharacters visually pop out of the literal filename text while
// typing, so it's obvious at a glance which part of the pattern is a
// literal match vs. a wildcard/anchor/capture.
function escapeRegexPreviewHtml(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
}
// Literal characters get an explicit light color too (#c9d4c9, the same
// "literal" shade classifyFilenameHtml()'s colorFor table already uses
// below) — not just the metacharacters. The readout Text this feeds has
// no color set of its own (textFormat: Text.RichText spans control it
// entirely), so an unstyled literal character fell through to Qt Quick's
// default black, nearly invisible against this popup's dark background.
function highlightPattern(pattern) {
    const meta = ".^$*+?()[]{}|\\"
    let out = ""
    for (let i = 0; i < pattern.length; i++) {
        const c = pattern[i]
        out += meta.indexOf(c) >= 0
                ? "<span style=\"color:#e8a33d; font-weight:bold;\">" + escapeRegexPreviewHtml(c) + "</span>"
                : "<span style=\"color:#c9d4c9;\">" + escapeRegexPreviewHtml(c) + "</span>"
    }
    return out
}

// Footgun guard: `|` (alternation) has the LOWEST precedence of any
// regex operator — lower than concatenation, lower than ^/$ — so a `|`
// not wrapped in its own group spans the ENTIRE pattern on either side,
// not just the neighboring token. E.g. `^TX0._MIC*._(edit)|(orig)` really
// parses as two fully independent top-level alternatives
// `^TX0._MIC*._(edit)` OR `(orig)` — and the second has no anchor, so it
// matches "orig" anywhere in any filename. Scans left to right tracking:
// backslash-escapes (an escaped `|`/`(`/`)`/`[`/`]` never counts as the
// metacharacter), character-class depth (a `|` or paren inside `[...]`
// is a literal, and classes never nest, so a single bool suffices), and
// paren depth (incremented on an unescaped `(` outside a class,
// decremented on an unescaped `)`). Returns true iff an unescaped `|` is
// found outside any character class AND at paren depth 0 — i.e. a
// top-level alternation not contained in its own group.
function hasUngroupedAlternation(pattern) {
    let parenDepth = 0
    let inClass = false
    let escaped = false
    for (let i = 0; i < pattern.length; i++) {
        const c = pattern[i]
        if (escaped) {
            escaped = false
            continue
        }
        if (c === "\\") {
            escaped = true
            continue
        }
        if (inClass) {
            if (c === "]")
                inClass = false
            continue
        }
        if (c === "[") {
            inClass = true
            continue
        }
        if (c === "(") {
            parenDepth++
            continue
        }
        if (c === ")") {
            parenDepth = Math.max(0, parenDepth - 1)
            continue
        }
        if (c === "|" && parenDepth === 0)
            return true
    }
    return false
}

// Per-filename match coloring for the combine-preview list: splits a
// filename into runs classified as literally matched, matched via a
// regex special construct (wildcard/class/quantifier/group), or outside
// the pattern's match entirely — so it's visible at a glance which part
// of a real filename each bit of the pattern actually accounted for.
//
// Approach: tokenize the pattern at depth 0 (outside classes/groups),
// classing each atom literal-or-special (quantifiers always force
// special, since the matched length becomes variable even for a literal
// char; whole groups are opaque leaves, classed literal only if their
// content is itself pure-literal with no alternation). Then build a
// "measurement" copy of the pattern where every token is wrapped in its
// own capturing group (any capturing parens already inside a token's own
// text are neutralized first so group numbering stays 1:1 with the
// token list), exec it against the filename, and recover each token's
// matched substring positionally (searching forward from a cursor that
// only advances) rather than via match.indices/the 'd' flag, which may
// not exist in this QML JS engine.
function findClassEnd(str, start) {
    let j = start + 1
    if (str[j] === "^")
        j++
    if (str[j] === "]")
        j++
    while (j < str.length && str[j] !== "]") {
        if (str[j] === "\\")
            j++
        j++
    }
    return Math.min(j + 1, str.length)
}
function findGroupEnd(str, start) {
    let depth = 0
    let j = start
    while (j < str.length) {
        const c = str[j]
        if (c === "\\") {
            j += 2
            continue
        }
        if (c === "[") {
            j = findClassEnd(str, j)
            continue
        }
        if (c === "(") {
            depth++
            j++
            continue
        }
        if (c === ")") {
            depth--
            j++
            if (depth === 0)
                return j
            continue
        }
        j++
    }
    return j
}
function readQuantifierEnd(str, start) {
    if (start >= str.length)
        return start
    const c = str[start]
    if (c === "*" || c === "+" || c === "?")
        return str[start + 1] === "?" ? start + 2 : start + 1
    if (c === "{") {
        const m = str.slice(start).match(/^\{\d+(,\d*)?\}\??/)
        if (m)
            return start + m[0].length
    }
    return start
}
// Rewrites `(` to `(?:` everywhere it isn't already `(?...`, so a
// token's own text can be wrapped in a fresh capturing group without
// its internal parens (if any, e.g. a whole matched group-atom) adding
// extra numbered groups that would desync classifications[] from the
// measurement pattern's real capture-group indices.
function neutralizeCapturingGroups(str) {
    let out = ""
    let i = 0
    while (i < str.length) {
        const c = str[i]
        if (c === "\\") {
            out += str.slice(i, i + 2)
            i += 2
            continue
        }
        if (c === "[") {
            const end = findClassEnd(str, i)
            out += str.slice(i, end)
            i = end
            continue
        }
        if (c === "(" && str[i + 1] !== "?") {
            out += "(?:"
            i++
            continue
        }
        out += c
        i++
    }
    return out
}
function tokenizePatternTopLevel(str) {
    const tokens = []
    let hasTopLevelAlternation = false
    let i = 0
    while (i < str.length) {
        const c = str[i]
        if (c === "^" || c === "$") {
            tokens.push({kind: "anchor", text: c})
            i++
            continue
        }
        if (c === "|") {
            hasTopLevelAlternation = true
            tokens.push({kind: "alt", text: "|"})
            i++
            continue
        }
        const atomStart = i
        let atomKind = "literal"
        if (c === "\\") {
            const next = str[i + 1]
            if (next === undefined) {
                i++
                continue
            }
            if ("dDwWsSbB".indexOf(next) >= 0)
                atomKind = "special"
            i += 2
        } else if (c === ".") {
            atomKind = "special"
            i++
        } else if (c === "[") {
            atomKind = "special"
            i = findClassEnd(str, i)
        } else if (c === "(") {
            const groupEnd = findGroupEnd(str, i)
            const inner = str.slice(i + 1, groupEnd - 1).replace(/^\?:/, "").replace(/^\?<[^>]+>/, "")
            const innerResult = tokenizePatternTopLevel(inner)
            const innerIsPureLiteral = !innerResult.hasTopLevelAlternation
                    && innerResult.tokens.every(function (t) { return t.kind === "literal" || t.kind === "anchor" })
            atomKind = innerIsPureLiteral ? "literal" : "special"
            i = groupEnd
        } else {
            i++
        }
        const qEnd = readQuantifierEnd(str, i)
        const quantified = qEnd > i
        i = qEnd
        tokens.push({kind: quantified ? "special" : atomKind, text: str.slice(atomStart, i)})
    }
    return {tokens: tokens, hasTopLevelAlternation: hasTopLevelAlternation}
}
function mergeLiteralTokens(tokens) {
    const merged = []
    for (const t of tokens) {
        if (t.kind === "literal" && merged.length > 0 && merged[merged.length - 1].kind === "literal")
            merged[merged.length - 1].text += t.text
        else
            merged.push({kind: t.kind, text: t.text})
    }
    return merged
}

// Fixed-order categorical palette for coloring each differentiator (real
// capturing group) distinctly in classifyFilenameHtml()'s preview and in
// an ON differentiator button's own highlight — so the button and every
// filename span it colors visibly pair up, which doubles as this
// palette's accessibility mitigation (see below). Slot order/values are
// the dataviz skill's dark-mode categorical theme (references/palette.md),
// validated against this popup's actual preview background via
// `node scripts/validate_palette.js "<hexes>" --mode dark --surface "#1e1e1e"`:
// lightness band, chroma floor, and contrast all PASS; worst adjacent CVD
// separation is a ΔE 10.3 WARN (8-12 floor band) — legal only with
// secondary encoding, satisfied here by the button/text pairing acting as
// a built-in per-color label (a colorblind user can still identify which
// differentiator is which by the button's own regex-snippet text, not
// color alone). Never reorder these — order is the CVD-safety mechanism,
// not cosmetic. A 9th+ differentiator wraps around rather than growing
// the palette (regex rules with >8 differentiators are not a case worth
// designing fresh colors for).
const differentiatorPalette = ["#3987e5", "#199e70", "#c98500", "#008300",
                               "#9085e9", "#e66767", "#d55181", "#d95926"]
function differentiatorColor(groupIndex) {
    return differentiatorPalette[(groupIndex - 1) % differentiatorPalette.length]
}

// --- Differentiator buttons ------------------------------------------
//
// A "differentiator" (see the combine-rule editor UI below) is really
// just a real capturing group: matchRuleCapturedKey()
// (TranscriptionController.cpp) already builds the combine-grouping key
// by concatenating every capture group 1..N in a matched filename, and
// a rule with zero capture groups puts every match in one bucket. So
// "toggle this pattern segment into a differentiator" is purely a text
// transform on the rule's own pattern string (wrap it in `(...)` to turn
// it on, `(?:...)` to turn it off) — the matching engine itself needs no
// changes at all.
//
// True capturing-group numbering (used both to know which run is
// already "on" and to ask the controller whether a given group always
// participates — see differentiatorNonParticipatingFiles() below) is
// NOT the same thing as tokenizePatternTopLevel()'s literal/special
// classification: a group with purely literal content, e.g. `(orig)`,
// classifies as "literal" (gets no button — nothing to differentiate,
// it's always the same text) but is STILL a real capturing group to
// QRegularExpression. So group numbering is derived independently, by
// scanning the pattern text for real capturing-group opens.

// True iff `pattern[i]` opens a REAL capturing group — plain "(" or a
// named group "(?<name>...)" — as opposed to a non-capturing "(?:", a
// lookahead "(?="/"(?!", or a lookbehind "(?<="/"(?<!". Mirrors
// tokenizePatternTopLevel()'s own recognition of "(?<name>...)" groups
// when extracting their inner content for classification.
function isCapturingGroupOpen(pattern, i) {
    if (pattern[i] !== "(")
        return false
    if (pattern[i + 1] !== "?")
        return true
    return pattern[i + 2] === "<" && pattern[i + 3] !== "=" && pattern[i + 3] !== "!"
}

// 1-based real capture-group number of the group opening at exactly
// `openParenIndex` in `pattern`, by a left-to-right scan counting every
// real capturing-group open (see isCapturingGroupOpen()) — the same
// numbering QRegularExpression itself uses, regardless of nesting depth
// or whether a group's own content happens to be literal. Returns -1 if
// `openParenIndex` isn't actually a capturing-group open.
function capturingGroupIndexAt(pattern, openParenIndex) {
    let count = 0
    let i = 0
    let inClass = false
    while (i < pattern.length) {
        const c = pattern[i]
        if (c === "\\") {
            i += 2
            continue
        }
        if (inClass) {
            if (c === "]")
                inClass = false
            i++
            continue
        }
        if (c === "[") {
            inClass = true
            i++
            continue
        }
        if (c === "(") {
            const capturing = isCapturingGroupOpen(pattern, i)
            if (capturing)
                count++
            if (i === openParenIndex)
                return capturing ? count : -1
        }
        i++
    }
    return -1
}

// Merges consecutive "special" (non-exact-match) tokens from
// tokenizePatternTopLevel() into single runs — mirrors
// mergeLiteralTokens() above, but for "special" instead of "literal".
// Anchors, top-level "|", and literal tokens all break a run rather
// than being absorbed into it, so a differentiator button never spans
// across one of those.
function mergeSpecialRuns(tokens) {
    const merged = []
    for (const t of tokens) {
        if (t.kind === "special" && merged.length > 0 && merged[merged.length - 1].kind === "run") {
            merged[merged.length - 1].text += t.text
            merged[merged.length - 1].tokenCount += 1
        } else if (t.kind === "special") {
            merged.push({kind: "run", text: t.text, tokenCount: 1})
        } else {
            merged.push({kind: t.kind, text: t.text, tokenCount: 1})
        }
    }
    return merged
}

// The clickable differentiator runs for `pattern`: each maximal
// consecutive span of "special" tokens, with its [start, end) character
// offsets (reconstructed by summing preceding token/run text lengths —
// tokenizePatternTopLevel()'s tokens fully partition the pattern with
// no gaps) and whether it's already a differentiator (a single-token
// run that's already a real capturing group — see isCapturingGroupOpen()).
function findDifferentiatorRuns(pattern) {
    const tokens = tokenizePatternTopLevel(pattern).tokens
    const runs = mergeSpecialRuns(tokens)
    const result = []
    let offset = 0
    for (const r of runs) {
        if (r.kind === "run") {
            const isDifferentiator = r.tokenCount === 1 && isCapturingGroupOpen(r.text, 0)
            result.push({start: offset, end: offset + r.text.length, text: r.text,
                         tokenCount: r.tokenCount, isDifferentiator: isDifferentiator})
        }
        offset += r.text.length
    }
    return result
}

// Re-derives the live capture-group index of a rule's sort
// differentiator (see findDifferentiatorRuns()/toggleDifferentiator())
// against `pattern`, by matching the run's own literal text
// (`sortGroupText`) rather than trusting a stored index. Capture-group
// numbering is positional — toggling an EARLIER differentiator on/off
// (or hand-editing the pattern text directly, which the raw
// rulePatternField always allows) shifts every later group's index — so
// a bare persisted integer would silently drift onto the wrong group
// the next time the pattern changes. Matching by text instead keeps the
// designation correct across edits.
//
// Text alone is ambiguous when two differentiator runs are
// byte-identical (e.g. a device-id group and a part-number group both
// written as plain `(\d+)` — an entirely realistic pattern for this
// feature's own target use case, not a rare corner case), so
// `sortGroupOccurrence` (0-based) disambiguates: "the Nth differentiator
// run, left to right, whose text is sortGroupText" — computed once at
// selection time (see toggleDifferentiator()) and re-applied identically
// here on every resolve. This is exact as long as no NEW run sharing
// that same text gets inserted BEFORE the marked one between commits
// (which would shift its occurrence rank without changing its identity)
// — a narrow, accepted residual limitation, same spirit as
// computeRuleOverlaps()'s "regex disjointness is undecidable in
// general" pragmatism. Returns -1 if sortGroupText is empty or no
// longer identifies a real differentiator run at that occurrence in
// `pattern` (e.g. the user deleted or de-differentiated it) — the
// caller then knows to fall back to the recording-mtime default.
function resolveSortGroupIndex(pattern, sortGroupText, sortGroupOccurrence) {
    if (!sortGroupText)
        return -1
    const sameText = findDifferentiatorRuns(pattern).filter(function (r) {
        return r.isDifferentiator && r.text === sortGroupText
    })
    const match = sameText[sortGroupOccurrence || 0]
    return match ? capturingGroupIndexAt(pattern, match.start) : -1
}

// Toggles a single differentiator run's capturing state and returns the
// FULL updated pattern string. Toggling OFF never fully strips a group's
// parens back to bare text — only flips "(" to "(?:" in place — since
// that could change match semantics for quantified/alternated content
// (e.g. `(a|b)+` -> `a|b+` is NOT equivalent, the same footgun
// hasUngroupedAlternation() above guards against). Toggling ON reuses
// neutralizeCapturingGroups() so any capturing parens already nested in
// the run's own text (e.g. an already-grouped sub-atom pulled into a
// multi-token run) don't separately contribute to
// matchRuleCapturedKey()'s concatenated key once the new outer group
// wraps them.
function toggleDifferentiatorRun(pattern, run) {
    const before = pattern.slice(0, run.start)
    const after = pattern.slice(run.end)
    let newText
    if (run.isDifferentiator)
        newText = "(?:" + run.text.slice(1)
    else if (run.tokenCount === 1 && run.text.startsWith("(?:"))
        newText = "(" + run.text.slice(3)
    else
        newText = "(" + neutralizeCapturingGroups(run.text) + ")"
    return before + newText + after
}

function classifyFilenameHtml(pattern, fileName) {
    if (!pattern)
        return escapeRegexPreviewHtml(fileName)

    const tokens = mergeLiteralTokens(
                tokenizePatternTopLevel(pattern).tokens)
    let measurementPattern = ""
    const classifications = []
    for (const t of tokens) {
        if (t.kind === "anchor" || t.kind === "alt") {
            measurementPattern += t.text
            continue
        }
        classifications.push(t.kind)
        measurementPattern += "(" + neutralizeCapturingGroups(t.text) + ")"
    }

    let re
    try {
        re = new RegExp(measurementPattern)
    } catch (e) {
        return escapeRegexPreviewHtml(fileName)
    }
    const m = re.exec(fileName)
    const classes = new Array(fileName.length).fill(null)
    if (m) {
        let cursor = 0
        for (let g = 0; g < classifications.length; g++) {
            const captured = m[g + 1]
            if (captured === undefined || captured.length === 0)
                continue
            const posInMatch = m[0].indexOf(captured, cursor)
            if (posInMatch < 0)
                continue
            const start = m.index + posInMatch
            const end = start + captured.length
            for (let k = start; k < end; k++)
                classes[k] = classifications[g]
            cursor = posInMatch + captured.length
        }
    }

    // Overlay: each REAL capturing group in the pattern AS AUTHORED (a
    // differentiator — see toggleDifferentiatorRun()'s doc comment; this
    // includes any hand-typed literal-content group like `(orig)`, just
    // as real a capture to matchRuleCapturedKey() as a toggled-on
    // button) gets its own color from differentiatorPalette, keyed by
    // its 1-based capture-group number — so two filenames that would
    // actually combine under this rule visibly share the same colored
    // text in every differentiator span, and diverge wherever they
    // wouldn't combine. Matched against `pattern` directly (the real
    // capturing parens are already there) rather than re-deriving via
    // the measurement-pattern trick above — same "good enough for a
    // preview, not the real combine decision" QML/JS RegExp tradeoff
    // this function already makes for the literal/special coloring.
    const diffGroup = new Array(fileName.length).fill(0)
    let realRe = null
    try {
        realRe = new RegExp(pattern)
    } catch (e) {
        // invalid pattern mid-typing — leave realRe null, no overlay
    }
    if (realRe) {
        const dm = realRe.exec(fileName)
        if (dm) {
            let dCursor = 0
            for (let g = 1; g < dm.length; g++) {
                const captured = dm[g]
                if (captured === undefined || captured.length === 0)
                    continue
                const posInMatch = dm[0].indexOf(captured, dCursor)
                if (posInMatch < 0)
                    continue
                const start = dm.index + posInMatch
                const end = start + captured.length
                for (let k = start; k < end; k++)
                    diffGroup[k] = g
                dCursor = posInMatch + captured.length
            }
        }
    }

    const colorFor = {literal: "#c9d4c9", special: "#e8a33d", none: "#666666"}
    function colorAt(idx) {
        return diffGroup[idx] > 0
                ? differentiatorColor(diffGroup[idx])
                : colorFor[classes[idx] || "none"]
    }
    let html = ""
    let runStart = 0
    let runColor = fileName.length > 0 ? colorAt(0) : ""
    for (let idx = 1; idx <= fileName.length; idx++) {
        const currentColor = idx < fileName.length ? colorAt(idx) : null
        if (currentColor !== runColor) {
            html += "<span style=\"color:" + runColor + ";\">"
                    + escapeRegexPreviewHtml(fileName.slice(runStart, idx)) + "</span>"
            runStart = idx
            runColor = currentColor
        }
    }
    return html
}
