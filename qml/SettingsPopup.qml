import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SuopTranscriber

Popup {
    id: settingsPopup

    signal changeVaultRequested()

    // Combine-rule "test this rule" preview: detectCombineGroups() lives on
    // the controller, the vault's current file listing on the tree model —
    // neither of which this popup otherwise touches.
    property var transcriptionController: null
    property var vaultTreeModel: null

    // Rule-list-level overlap warnings (see computeRuleOverlaps()'s doc
    // comment). Deliberately NOT a plain `property var x: <binding>` —
    // vaultTreeModel.allAudioPaths() is a Q_INVOKABLE with no NOTIFY
    // signal, so QML's binding engine can't track "the vault's file list
    // changed" as a dependency at all, and on a freshly opened vault
    // AppSettings.loadFromVault() (which sets combineRules, the only real
    // tracked dependency here) runs BEFORE VaultTreeModel::rebuild()
    // populates that file list (see Main.qml's own doc comment on that
    // ordering) — so a declarative binding here would evaluate once, too
    // early, against an empty file list, and then never again (same
    // "frozen at initial value" class of bug as the dummy-dependency
    // trick elsewhere in this codebase, just via a genuinely-untracked
    // invokable instead). Recomputed imperatively instead: once whenever
    // this popup actually opens (by then the vault a real user is looking
    // at is definitely already loaded), and again on every combineRules
    // change while open (rule add/edit/differentiator toggle).
    property var combineOverlapWarnings: []
    function updateCombineOverlapWarnings() {
        settingsPopup.combineOverlapWarnings = settingsPopup.vaultTreeModel
                ? settingsPopup.computeRuleOverlaps(AppSettings.combineRules, settingsPopup.vaultTreeModel.allAudioPaths())
                : []
    }
    onOpened: settingsPopup.updateCombineOverlapWarnings()
    Connections {
        target: AppSettings
        function onCombineRulesChanged() {
            if (settingsPopup.opened)
                settingsPopup.updateCombineOverlapWarnings()
        }
    }

    width: 440
    height: Math.min(contentColumn.implicitHeight + 32, maxHeight)
    // Modal: Overlay.overlay's automatic block-plane now genuinely blocks
    // clicks/drags to Sidebar/MainPanel/BottomBar/TitleBar while this is
    // open (a previous `modal: false` + input-swallowing MouseArea
    // workaround in Main.qml correctly blocked hover but let TapHandler
    // clicks pass through — TapHandlers use passive-grab semantics
    // independent of that MouseArea's exclusive Item-grab). The window-edge
    // ResizeGrips (Main.qml) stay usable despite the block-plane covering
    // the whole window: they're reparented onto Overlay.overlay itself with
    // a z above the popup's own block-plane/content, so they keep
    // painting/receiving input on top of it. `dim` is still set explicitly
    // below (not left to just follow `modal`) for the slider-drag
    // passthrough behavior.
    modal: true
    focus: true
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0
    // Suspended (NoAutoClose) while middleDragScrollArea is armed: Escape
    // and click-outside are how Chrome-style autoscroll cancels itself too,
    // and without this the popup's own CloseOnEscape/CloseOnPressOutside
    // would race the autoscroll's own cancel handling and close the whole
    // Settings popup instead of just disarming the scroll.
    closePolicy: middleDragScrollArea.armed
                 ? Popup.NoAutoClose
                 : (Popup.CloseOnEscape | Popup.CloseOnPressOutside)

    readonly property real maxHeight: parent ? parent.height * 0.9 : 600

    // While any slider in here is actively being dragged, hide this popup's
    // modal dimmer and its own visual content so the user can see the live
    // effect on the app behind it — reappears the instant the drag ends.
    // The Popup itself stays open/modal (visible/opacity unchanged) so the
    // slider's mouse grab isn't interrupted mid-drag.
    property bool anyPressed: radiusSlider.pressed || sidebarOpacitySlider.pressed
                               || mainOpacitySlider.pressed || rimWidthSlider.pressed
                               || highlightRadiusSlider.pressed || tagReuseBiasSlider.pressed
                               || timestampDecimalPlacesSlider.pressed
                               || progressGlowWidthSlider.pressed || progressGlowRadiusSlider.pressed

    dim: !anyPressed

    // Covers every key in AppSettings.defaults; used to enable/disable the
    // global "Reset to defaults" button. Reactive because it only reads
    // AppSettings properties (via isDefault), no manual signal wiring needed.
    function anySettingDirty() {
        return !AppSettings.isDefault("borderRadius")
                || !AppSettings.isDefault("useSystemTitlebar")
                || !AppSettings.isDefault("sidebarBackgroundColor")
                || !AppSettings.isDefault("sidebarOpacity")
                || !AppSettings.isDefault("sidebarBlur")
                || !AppSettings.isDefault("mainBackgroundColor")
                || !AppSettings.isDefault("mainOpacity")
                || !AppSettings.isDefault("mainBlur")
                || !AppSettings.isDefault("rimEnabled")
                || !AppSettings.isDefault("rimWidth")
                || !AppSettings.isDefault("rimColor")
                || !AppSettings.isDefault("highlightColor")
                || !AppSettings.isDefault("highlightRadius")
                || !AppSettings.isDefault("sidebarSelectionColor")
                || !AppSettings.isDefault("sidebarSelectionBorderColor")
                || !AppSettings.isDefault("useLabelsMainPanel")
                || !AppSettings.isDefault("useLabelsSidebar")
                || !AppSettings.isDefault("hideExtensionsSidebar")
                || !AppSettings.isDefault("hideExtensionsMainPanel")
                || !AppSettings.isDefault("autoLabelEnabled")
                || !AppSettings.isDefault("restrictTagsToExisting")
                || !AppSettings.isDefault("tagReuseBias")
                || !AppSettings.isDefault("timestampDecimalPlaces")
                || !AppSettings.isDefault("progressGlowEnabled")
                || !AppSettings.isDefault("progressGlowColor")
                || !AppSettings.isDefault("progressGlowWidth")
                || !AppSettings.isDefault("progressGlowRadius")
                || !AppSettings.isDefault("deleteOriginalsAfterCombine")
    }

    // Rule mutation helpers: AppSettings.combineRules is a plain JS array
    // (see its own doc comment in AppSettings.qml — no per-value reset
    // semantics fit it), so every edit rebuilds and reassigns the whole
    // array to trigger combineRulesChanged.
    function updateRule(index, key, value) {
        const rules = AppSettings.combineRules.slice()
        rules[index] = Object.assign({}, rules[index], {[key]: value})
        AppSettings.combineRules = rules
    }
    // The ONLY writer of rule `pattern` — every pattern edit (button-driven
    // differentiator toggle, or hand-typed text in rulePatternField) must
    // route through here rather than a plain updateRule(index, "pattern",
    // ...), because it also re-derives/heals sortGroupIndex against the NEW
    // pattern (see resolveSortGroupIndex()'s doc comment for why a stored
    // index alone can't survive pattern edits, and why sortGroupOccurrence
    // rides alongside sortGroupText to disambiguate duplicate-text runs).
    // `overrideSortGroupText`/`overrideSortGroupOccurrence`, if given,
    // REPLACE the rule's current sort-differentiator designation (used when
    // the user just picked a different run to sort by, or cleared it with
    // ""); omitted, they keep whatever the rule already had, just
    // re-resolved against `newPattern`.
    function commitPatternChange(index, newPattern, overrideSortGroupText, overrideSortGroupOccurrence) {
        const rule = AppSettings.combineRules[index]
        const targetText = overrideSortGroupText !== undefined ? overrideSortGroupText : (rule.sortGroupText || "")
        const targetOccurrence = overrideSortGroupText !== undefined
                ? (overrideSortGroupOccurrence || 0) : (rule.sortGroupOccurrence || 0)
        const sortGroupIndex = settingsPopup.resolveSortGroupIndex(newPattern, targetText, targetOccurrence)
        const rules = AppSettings.combineRules.slice()
        rules[index] = Object.assign({}, rules[index], {
            pattern: newPattern,
            sortGroupText: sortGroupIndex > 0 ? targetText : "",
            sortGroupOccurrence: sortGroupIndex > 0 ? targetOccurrence : 0,
            sortGroupIndex: sortGroupIndex
        })
        AppSettings.combineRules = rules
    }
    function addRule() {
        AppSettings.combineRules = AppSettings.combineRules.concat(
                    [{name: qsTr("New rule"), pattern: "", enabled: true}])
    }
    function removeRule(index) {
        const rules = AppSettings.combineRules.slice()
        rules.splice(index, 1)
        AppSettings.combineRules = rules
    }

    // Not a full syntax highlighter — just enough to make regex
    // metacharacters visually pop out of the literal filename text while
    // typing, so it's obvious at a glance which part of the pattern is a
    // literal match vs. a wildcard/anchor/capture.
    function escapeRegexPreviewHtml(s) {
        return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }
    function highlightPattern(pattern) {
        const meta = ".^$*+?()[]{}|\\"
        let out = ""
        for (let i = 0; i < pattern.length; i++) {
            const c = pattern[i]
            out += meta.indexOf(c) >= 0
                    ? "<span style=\"color:#e8a33d; font-weight:bold;\">" + escapeRegexPreviewHtml(c) + "</span>"
                    : escapeRegexPreviewHtml(c)
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
                j = settingsPopup.findClassEnd(str, j)
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
                const end = settingsPopup.findClassEnd(str, i)
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
                i = settingsPopup.findClassEnd(str, i)
            } else if (c === "(") {
                const groupEnd = settingsPopup.findGroupEnd(str, i)
                const inner = str.slice(i + 1, groupEnd - 1).replace(/^\?:/, "").replace(/^\?<[^>]+>/, "")
                const innerResult = settingsPopup.tokenizePatternTopLevel(inner)
                const innerIsPureLiteral = !innerResult.hasTopLevelAlternation
                        && innerResult.tokens.every(function (t) { return t.kind === "literal" || t.kind === "anchor" })
                atomKind = innerIsPureLiteral ? "literal" : "special"
                i = groupEnd
            } else {
                i++
            }
            const qEnd = settingsPopup.readQuantifierEnd(str, i)
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
    readonly property var differentiatorPalette: ["#3987e5", "#199e70", "#c98500", "#008300",
                                                   "#9085e9", "#e66767", "#d55181", "#d95926"]
    function differentiatorColor(groupIndex) {
        return settingsPopup.differentiatorPalette[(groupIndex - 1) % settingsPopup.differentiatorPalette.length]
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
                const capturing = settingsPopup.isCapturingGroupOpen(pattern, i)
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
        const tokens = settingsPopup.tokenizePatternTopLevel(pattern).tokens
        const runs = settingsPopup.mergeSpecialRuns(tokens)
        const result = []
        let offset = 0
        for (const r of runs) {
            if (r.kind === "run") {
                const isDifferentiator = r.tokenCount === 1 && settingsPopup.isCapturingGroupOpen(r.text, 0)
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
        const sameText = settingsPopup.findDifferentiatorRuns(pattern).filter(function (r) {
            return r.isDifferentiator && r.text === sortGroupText
        })
        const match = sameText[sortGroupOccurrence || 0]
        return match ? settingsPopup.capturingGroupIndexAt(pattern, match.start) : -1
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
            newText = "(" + settingsPopup.neutralizeCapturingGroups(run.text) + ")"
        return before + newText + after
    }

    // Non-blocking "these rules might conflict" warnings for the combine-
    // rule list as a whole — general regex disjointness is undecidable for
    // QRegularExpression's backtracking dialect (see
    // hasUngroupedAlternation()'s doc comment for a taste of how gnarly
    // that gets), so this only catches what's actually checkable: (a) two
    // enabled rules with byte-identical pattern text (the "obvious" static
    // case — zero false positives, catches the copy-paste-forgot-to-edit
    // mistake), and (b) two enabled rules that empirically both match the
    // SAME file currently in the vault, reusing detectCombineGroups()'s
    // real matching (no new C++ needed — a file matched by 2+ rules today
    // silently gets queued into 2+ separate combine jobs, since
    // findCombineGroups() has no first-rule-wins dedup). Returns an object
    // keyed by ruleName -> array of {other: otherRuleName, file: "" for the
    // static case, or the shared filename for the empirical case}.
    function computeRuleOverlaps(rules, audioPaths) {
        const warnings = {}
        function addWarning(ruleName, other, file) {
            if (!warnings[ruleName])
                warnings[ruleName] = []
            warnings[ruleName].push({other: other, file: file})
        }

        const enabled = rules.filter(function (r) { return r.enabled && r.pattern.length > 0 })
        for (let i = 0; i < enabled.length; i++) {
            for (let j = i + 1; j < enabled.length; j++) {
                if (enabled[i].pattern === enabled[j].pattern) {
                    addWarning(enabled[i].name, enabled[j].name, "")
                    addWarning(enabled[j].name, enabled[i].name, "")
                }
            }
        }

        if (settingsPopup.transcriptionController && enabled.length > 0) {
            const groups = settingsPopup.transcriptionController.detectCombineGroups(enabled, audioPaths)
            const fileToRules = {}
            for (const g of groups) {
                for (const f of g.files) {
                    // detectCombineGroups() returns full paths — shorten to
                    // just the filename for display, matching every other
                    // file-listing spot in this popup (e.g. the preview
                    // cards' own f.substring(f.lastIndexOf("/") + 1) below,
                    // and differentiatorNonParticipatingFiles(), which
                    // already returns bare filenames from the C++ side).
                    const name = f.substring(f.lastIndexOf("/") + 1)
                    if (!fileToRules[name])
                        fileToRules[name] = []
                    if (fileToRules[name].indexOf(g.ruleName) < 0)
                        fileToRules[name].push(g.ruleName)
                }
            }
            for (const f in fileToRules) {
                const names = fileToRules[f]
                if (names.length > 1) {
                    for (let i = 0; i < names.length; i++)
                        for (let j = 0; j < names.length; j++)
                            if (i !== j)
                                addWarning(names[i], names[j], f)
                }
            }
        }
        return warnings
    }
    function classifyFilenameHtml(pattern, fileName) {
        if (!pattern)
            return settingsPopup.escapeRegexPreviewHtml(fileName)

        const tokens = settingsPopup.mergeLiteralTokens(
                    settingsPopup.tokenizePatternTopLevel(pattern).tokens)
        let measurementPattern = ""
        const classifications = []
        for (const t of tokens) {
            if (t.kind === "anchor" || t.kind === "alt") {
                measurementPattern += t.text
                continue
            }
            classifications.push(t.kind)
            measurementPattern += "(" + settingsPopup.neutralizeCapturingGroups(t.text) + ")"
        }

        let re
        try {
            re = new RegExp(measurementPattern)
        } catch (e) {
            return settingsPopup.escapeRegexPreviewHtml(fileName)
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
                    ? settingsPopup.differentiatorColor(diffGroup[idx])
                    : colorFor[classes[idx] || "none"]
        }
        let html = ""
        let runStart = 0
        let runColor = fileName.length > 0 ? colorAt(0) : ""
        for (let idx = 1; idx <= fileName.length; idx++) {
            const currentColor = idx < fileName.length ? colorAt(idx) : null
            if (currentColor !== runColor) {
                html += "<span style=\"color:" + runColor + ";\">"
                        + settingsPopup.escapeRegexPreviewHtml(fileName.slice(runStart, idx)) + "</span>"
                runStart = idx
                runColor = currentColor
            }
        }
        return html
    }

    background: Rectangle {
        color: "#242424"
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: "#3a3a3a"
        opacity: settingsPopup.anyPressed ? 0 : 1
    }

    ColorDialog {
        id: sidebarColorDialog
        title: qsTr("Sidebar Background Color")
        selectedColor: AppSettings.sidebarBackgroundColor
        onAccepted: AppSettings.sidebarBackgroundColor = selectedColor
    }

    ColorDialog {
        id: mainColorDialog
        title: qsTr("Main Panel Background Color")
        selectedColor: AppSettings.mainBackgroundColor
        onAccepted: AppSettings.mainBackgroundColor = selectedColor
    }

    ColorDialog {
        id: rimColorDialog
        title: qsTr("Rim Color")
        selectedColor: AppSettings.rimColor
        options: ColorDialog.ShowAlphaChannel
        onAccepted: AppSettings.rimColor = selectedColor
    }

    ColorDialog {
        id: highlightColorDialog
        title: qsTr("Sentence Highlight Color")
        selectedColor: AppSettings.highlightColor
        onAccepted: AppSettings.highlightColor = selectedColor
    }

    ColorDialog {
        id: sidebarSelectionColorDialog
        title: qsTr("Sidebar Selection Color")
        selectedColor: AppSettings.sidebarSelectionColor
        options: ColorDialog.ShowAlphaChannel
        onAccepted: AppSettings.sidebarSelectionColor = selectedColor
    }

    ColorDialog {
        id: sidebarSelectionBorderColorDialog
        title: qsTr("Sidebar Selection Border Color")
        selectedColor: AppSettings.sidebarSelectionBorderColor
        options: ColorDialog.ShowAlphaChannel
        onAccepted: AppSettings.sidebarSelectionBorderColor = selectedColor
    }

    ColorDialog {
        id: progressGlowColorDialog
        title: qsTr("Progress Glow Color")
        selectedColor: AppSettings.progressGlowColor
        options: ColorDialog.ShowAlphaChannel
        onAccepted: AppSettings.progressGlowColor = selectedColor
    }

    ScrollView {
        id: scrollArea
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.topMargin: 16
        anchors.bottomMargin: 16
        // Reduced by the same 14px contentColumn reserves below for the
        // scrollbar overlay — keeps the CONTENT symmetric (16px inset on
        // both sides) by giving the scrollbar its own lane inside the right
        // margin, instead of 14px of clearance stacking on top of a full
        // 16px margin and making the popup look lopsided.
        anchors.rightMargin: 2
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        opacity: settingsPopup.anyPressed ? 0 : 1

        ColumnLayout {
            id: contentColumn
            // NOT parent.width: a QML child declared directly inside
            // ScrollView sits in its internal Flickable's contentItem, whose
            // own width is otherwise driven BY this same child's implicit
            // size when nothing else constrains it — "width: parent.width"
            // is a binding loop that settles at contentColumn's own
            // (unclamped) implicitWidth instead of the real viewport width.
            // Binding to the ScrollView's own id breaks the loop: verified
            // empirically (offscreen qml repro) that parent.width let a
            // slider row's content stretch past the popup's actual bounds
            // even though every individual row fit under naive arithmetic.
            //
            // -14: the Basic style's vertical ScrollBar is an OVERLAY —
            // `parent: control; x: control.width - width` (confirmed in
            // /usr/lib/qt6/qml/QtQuick/Controls/Basic/ScrollView.qml) — it
            // reserves no layout space and floats flush against the
            // ScrollView's right edge, right on top of content that fills
            // the full width (e.g. the per-row reset buttons). Leaving a
            // clearance gap here keeps it from covering them.
            width: scrollArea.width - 14
            clip: true
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Settings")
                    color: "white"
                    font.bold: true
                    font.pixelSize: 16
                }
                Button {
                    id: resetAllButton
                    text: qsTr("Reset to defaults")
                    enabled: settingsPopup.anySettingDirty()
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetAll()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (resetAllButton.hovered && resetAllButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: resetAllButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: resetAllButton.enabled && resetAllButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Vault")
                }
                Button {
                    id: changeVaultButton
                    text: qsTr("Change Vault")
                    hoverEnabled: true
                    onClicked: settingsPopup.changeVaultRequested()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (changeVaultButton.hovered && changeVaultButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: changeVaultButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("Appearance")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Border radius")
                }
                Slider {
                    id: radiusSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 24
                    value: AppSettings.borderRadius
                    hoverEnabled: true
                    onMoved: AppSettings.borderRadius = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: radiusSlider.hovered
                    }
                }
                TextField {
                    id: radiusField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: radiusSlider.from; top: radiusSlider.to; decimals: 1 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.borderRadius = Math.max(radiusSlider.from, Math.min(radiusSlider.to, v))
                        text = AppSettings.borderRadius.toFixed(1)
                    }

                    background: UnderlineField { focused: radiusField.activeFocus }

                    Binding {
                        target: radiusField
                        property: "text"
                        value: AppSettings.borderRadius.toFixed(1)
                        when: !radiusField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: radiusField.hovered
                    }
                }
                Button {
                    id: radiusResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("borderRadius")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("borderRadius")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: radiusResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: radiusResetButton.enabled && radiusResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Use system titlebar")
                }
                Switch {
                    id: titlebarSwitch
                    checked: AppSettings.useSystemTitlebar
                    hoverEnabled: true
                    onToggled: AppSettings.useSystemTitlebar = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: titlebarSwitch.hovered
                    }
                }
                Button {
                    id: titlebarResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("useSystemTitlebar")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("useSystemTitlebar")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: titlebarResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: titlebarResetButton.enabled && titlebarResetButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("Sidebar")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Background color")
                }
                Button {
                    id: sidebarColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: sidebarColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (sidebarColorButton.hovered && sidebarColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarColorButton.hovered
                    }
                }
                Button {
                    id: sidebarColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("sidebarBackgroundColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("sidebarBackgroundColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: sidebarColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarColorResetButton.enabled && sidebarColorResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Opacity")
                }
                Slider {
                    id: sidebarOpacitySlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 1
                    value: AppSettings.sidebarOpacity
                    hoverEnabled: true
                    onMoved: AppSettings.sidebarOpacity = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarOpacitySlider.hovered
                    }
                }
                TextField {
                    id: sidebarOpacityField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: sidebarOpacitySlider.from; top: sidebarOpacitySlider.to; decimals: 2 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.sidebarOpacity = Math.max(sidebarOpacitySlider.from, Math.min(sidebarOpacitySlider.to, v))
                        text = AppSettings.sidebarOpacity.toFixed(2)
                    }

                    background: UnderlineField { focused: sidebarOpacityField.activeFocus }

                    Binding {
                        target: sidebarOpacityField
                        property: "text"
                        value: AppSettings.sidebarOpacity.toFixed(2)
                        when: !sidebarOpacityField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarOpacityField.hovered
                    }
                }
                Button {
                    id: sidebarOpacityResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("sidebarOpacity")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("sidebarOpacity")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: sidebarOpacityResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarOpacityResetButton.enabled && sidebarOpacityResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Blur")
                }
                Switch {
                    id: sidebarBlurSwitch
                    checked: AppSettings.sidebarBlur
                    hoverEnabled: true
                    onToggled: AppSettings.sidebarBlur = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarBlurSwitch.hovered
                    }
                }
                Button {
                    id: sidebarBlurResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("sidebarBlur")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("sidebarBlur")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: sidebarBlurResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarBlurResetButton.enabled && sidebarBlurResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Selection fill color")
                }
                Button {
                    id: sidebarSelectionColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: sidebarSelectionColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (sidebarSelectionColorButton.hovered && sidebarSelectionColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarSelectionColorButton.hovered
                    }
                }
                Button {
                    id: sidebarSelectionColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("sidebarSelectionColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("sidebarSelectionColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: sidebarSelectionColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarSelectionColorResetButton.enabled && sidebarSelectionColorResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Selection border color")
                }
                Button {
                    id: sidebarSelectionBorderColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: sidebarSelectionBorderColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (sidebarSelectionBorderColorButton.hovered && sidebarSelectionBorderColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarSelectionBorderColorButton.hovered
                    }
                }
                Button {
                    id: sidebarSelectionBorderColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("sidebarSelectionBorderColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("sidebarSelectionBorderColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: sidebarSelectionBorderColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarSelectionBorderColorResetButton.enabled && sidebarSelectionBorderColorResetButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("Main panel")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Background color")
                }
                Button {
                    id: mainColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: mainColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (mainColorButton.hovered && mainColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainColorButton.hovered
                    }
                }
                Button {
                    id: mainColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("mainBackgroundColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("mainBackgroundColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: mainColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainColorResetButton.enabled && mainColorResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Opacity")
                }
                Slider {
                    id: mainOpacitySlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 1
                    value: AppSettings.mainOpacity
                    hoverEnabled: true
                    onMoved: AppSettings.mainOpacity = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainOpacitySlider.hovered
                    }
                }
                TextField {
                    id: mainOpacityField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: mainOpacitySlider.from; top: mainOpacitySlider.to; decimals: 2 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.mainOpacity = Math.max(mainOpacitySlider.from, Math.min(mainOpacitySlider.to, v))
                        text = AppSettings.mainOpacity.toFixed(2)
                    }

                    background: UnderlineField { focused: mainOpacityField.activeFocus }

                    Binding {
                        target: mainOpacityField
                        property: "text"
                        value: AppSettings.mainOpacity.toFixed(2)
                        when: !mainOpacityField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainOpacityField.hovered
                    }
                }
                Button {
                    id: mainOpacityResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("mainOpacity")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("mainOpacity")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: mainOpacityResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainOpacityResetButton.enabled && mainOpacityResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Blur")
                }
                Switch {
                    id: mainBlurSwitch
                    checked: AppSettings.mainBlur
                    hoverEnabled: true
                    onToggled: AppSettings.mainBlur = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainBlurSwitch.hovered
                    }
                }
                Button {
                    id: mainBlurResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("mainBlur")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("mainBlur")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: mainBlurResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainBlurResetButton.enabled && mainBlurResetButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("Rim")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Enabled")
                }
                Switch {
                    id: rimEnabledSwitch
                    checked: AppSettings.rimEnabled
                    hoverEnabled: true
                    onToggled: AppSettings.rimEnabled = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimEnabledSwitch.hovered
                    }
                }
                Button {
                    id: rimEnabledResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("rimEnabled")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("rimEnabled")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: rimEnabledResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimEnabledResetButton.enabled && rimEnabledResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Width")
                }
                Slider {
                    id: rimWidthSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 8
                    value: AppSettings.rimWidth
                    hoverEnabled: true
                    onMoved: AppSettings.rimWidth = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimWidthSlider.hovered
                    }
                }
                TextField {
                    id: rimWidthField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: rimWidthSlider.from; top: rimWidthSlider.to; decimals: 1 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.rimWidth = Math.max(rimWidthSlider.from, Math.min(rimWidthSlider.to, v))
                        text = AppSettings.rimWidth.toFixed(1)
                    }

                    background: UnderlineField { focused: rimWidthField.activeFocus }

                    Binding {
                        target: rimWidthField
                        property: "text"
                        value: AppSettings.rimWidth.toFixed(1)
                        when: !rimWidthField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimWidthField.hovered
                    }
                }
                Button {
                    id: rimWidthResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("rimWidth")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("rimWidth")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: rimWidthResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimWidthResetButton.enabled && rimWidthResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Color")
                }
                Button {
                    id: rimColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: rimColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (rimColorButton.hovered && rimColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimColorButton.hovered
                    }
                }
                Button {
                    id: rimColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("rimColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("rimColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: rimColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: rimColorResetButton.enabled && rimColorResetButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("Sentence Highlight")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Color")
                }
                Button {
                    id: highlightColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: highlightColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (highlightColorButton.hovered && highlightColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: highlightColorButton.hovered
                    }
                }
                Button {
                    id: highlightColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("highlightColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("highlightColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: highlightColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: highlightColorResetButton.enabled && highlightColorResetButton.hovered
                    }
                }
            }

            // Item 7 (Wave E): playback + Ctrl+F search-match highlight radius,
            // now independently adjustable from the app-wide border radius
            // above (was previously hardcoded to follow it).
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Radius")
                }
                Slider {
                    id: highlightRadiusSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 24
                    value: AppSettings.highlightRadius
                    hoverEnabled: true
                    onMoved: AppSettings.highlightRadius = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: highlightRadiusSlider.hovered
                    }
                }
                TextField {
                    id: highlightRadiusField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: highlightRadiusSlider.from; top: highlightRadiusSlider.to; decimals: 1 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.highlightRadius = Math.max(highlightRadiusSlider.from, Math.min(highlightRadiusSlider.to, v))
                        text = AppSettings.highlightRadius.toFixed(1)
                    }

                    background: UnderlineField { focused: highlightRadiusField.activeFocus }

                    Binding {
                        target: highlightRadiusField
                        property: "text"
                        value: AppSettings.highlightRadius.toFixed(1)
                        when: !highlightRadiusField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: highlightRadiusField.hovered
                    }
                }
                Button {
                    id: highlightRadiusResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("highlightRadius")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("highlightRadius")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: highlightRadiusResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: highlightRadiusResetButton.enabled && highlightRadiusResetButton.hovered
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Applies to the playback sentence highlight and the Ctrl+F search-match highlight.")
                color: "#888888"
                font.pixelSize: 11
            }

            Label {
                text: qsTr("Progress Glow")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Enabled")
                }
                Switch {
                    id: progressGlowEnabledSwitch
                    checked: AppSettings.progressGlowEnabled
                    hoverEnabled: true
                    onToggled: AppSettings.progressGlowEnabled = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowEnabledSwitch.hovered
                    }
                }
                Button {
                    id: progressGlowEnabledResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("progressGlowEnabled")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("progressGlowEnabled")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: progressGlowEnabledResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowEnabledResetButton.enabled && progressGlowEnabledResetButton.hovered
                    }
                }
            }

            Label {
                visible: progressGlowEnabledSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Replaces the bottom info bar's progress bar with a thin glowing line across its top edge.")
                color: "#888888"
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                visible: progressGlowEnabledSwitch.checked
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Color")
                }
                Button {
                    id: progressGlowColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: progressGlowColorDialog.open()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (progressGlowColorButton.hovered && progressGlowColorButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowColorButton.hovered
                    }
                }
                Button {
                    id: progressGlowColorResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("progressGlowColor")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("progressGlowColor")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: progressGlowColorResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowColorResetButton.enabled && progressGlowColorResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: progressGlowEnabledSwitch.checked
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Width")
                }
                Slider {
                    id: progressGlowWidthSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 8
                    value: AppSettings.progressGlowWidth
                    hoverEnabled: true
                    onMoved: AppSettings.progressGlowWidth = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowWidthSlider.hovered
                    }
                }
                TextField {
                    id: progressGlowWidthField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: progressGlowWidthSlider.from; top: progressGlowWidthSlider.to; decimals: 1 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.progressGlowWidth = Math.max(progressGlowWidthSlider.from, Math.min(progressGlowWidthSlider.to, v))
                        text = AppSettings.progressGlowWidth.toFixed(1)
                    }

                    background: UnderlineField { focused: progressGlowWidthField.activeFocus }

                    Binding {
                        target: progressGlowWidthField
                        property: "text"
                        value: AppSettings.progressGlowWidth.toFixed(1)
                        when: !progressGlowWidthField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowWidthField.hovered
                    }
                }
                Button {
                    id: progressGlowWidthResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("progressGlowWidth")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("progressGlowWidth")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: progressGlowWidthResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowWidthResetButton.enabled && progressGlowWidthResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: progressGlowEnabledSwitch.checked
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Glow radius")
                }
                Slider {
                    id: progressGlowRadiusSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 32
                    value: AppSettings.progressGlowRadius
                    hoverEnabled: true
                    onMoved: AppSettings.progressGlowRadius = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowRadiusSlider.hovered
                    }
                }
                TextField {
                    id: progressGlowRadiusField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: DoubleValidator { bottom: progressGlowRadiusSlider.from; top: progressGlowRadiusSlider.to; decimals: 1 }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (!isNaN(v))
                            AppSettings.progressGlowRadius = Math.max(progressGlowRadiusSlider.from, Math.min(progressGlowRadiusSlider.to, v))
                        text = AppSettings.progressGlowRadius.toFixed(1)
                    }

                    background: UnderlineField { focused: progressGlowRadiusField.activeFocus }

                    Binding {
                        target: progressGlowRadiusField
                        property: "text"
                        value: AppSettings.progressGlowRadius.toFixed(1)
                        when: !progressGlowRadiusField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowRadiusField.hovered
                    }
                }
                Button {
                    id: progressGlowRadiusResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("progressGlowRadius")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("progressGlowRadius")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: progressGlowRadiusResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: progressGlowRadiusResetButton.enabled && progressGlowRadiusResetButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("Timestamps")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Decimal places")
                }
                Slider {
                    id: timestampDecimalPlacesSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 3
                    stepSize: 1
                    snapMode: Slider.SnapAlways
                    value: AppSettings.timestampDecimalPlaces
                    hoverEnabled: true
                    onMoved: AppSettings.timestampDecimalPlaces = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: timestampDecimalPlacesSlider.hovered
                    }
                }
                TextField {
                    id: timestampDecimalPlacesField
                    Layout.preferredWidth: 56
                    horizontalAlignment: TextInput.AlignRight
                    hoverEnabled: true
                    validator: IntValidator { bottom: timestampDecimalPlacesSlider.from; top: timestampDecimalPlacesSlider.to }
                    onEditingFinished: {
                        const v = parseInt(text, 10)
                        if (!isNaN(v))
                            AppSettings.timestampDecimalPlaces = Math.max(timestampDecimalPlacesSlider.from, Math.min(timestampDecimalPlacesSlider.to, v))
                        text = String(AppSettings.timestampDecimalPlaces)
                    }

                    background: UnderlineField { focused: timestampDecimalPlacesField.activeFocus }

                    Binding {
                        target: timestampDecimalPlacesField
                        property: "text"
                        value: String(AppSettings.timestampDecimalPlaces)
                        when: !timestampDecimalPlacesField.activeFocus
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: timestampDecimalPlacesField.hovered
                    }
                }
                Button {
                    id: timestampDecimalPlacesResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("timestampDecimalPlaces")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("timestampDecimalPlaces")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: timestampDecimalPlacesResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: timestampDecimalPlacesResetButton.enabled && timestampDecimalPlacesResetButton.hovered
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Rounds the transcript gutter's displayed timestamp only — the underlying data always keeps full precision.")
                color: "#888888"
                font.pixelSize: 11
            }

            Label {
                text: qsTr("Files")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Show labels instead of filenames (main panel)")
                }
                Switch {
                    id: useLabelsMainPanelSwitch
                    checked: AppSettings.useLabelsMainPanel
                    hoverEnabled: true
                    onToggled: AppSettings.useLabelsMainPanel = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: useLabelsMainPanelSwitch.hovered
                    }
                }
                Button {
                    id: useLabelsMainPanelResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("useLabelsMainPanel")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("useLabelsMainPanel")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: useLabelsMainPanelResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: useLabelsMainPanelResetButton.enabled && useLabelsMainPanelResetButton.hovered
                    }
                }
            }

            Label {
                visible: !useLabelsMainPanelSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Off: renaming a file in the main panel renames the actual audio + transcript files instead of just setting a label.")
                color: "#888888"
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Show labels instead of filenames (sidebar)")
                }
                Switch {
                    id: useLabelsSidebarSwitch
                    checked: AppSettings.useLabelsSidebar
                    hoverEnabled: true
                    onToggled: AppSettings.useLabelsSidebar = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: useLabelsSidebarSwitch.hovered
                    }
                }
                Button {
                    id: useLabelsSidebarResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("useLabelsSidebar")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("useLabelsSidebar")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: useLabelsSidebarResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: useLabelsSidebarResetButton.enabled && useLabelsSidebarResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Hide file extensions (sidebar)")
                }
                Switch {
                    id: hideExtensionsSidebarSwitch
                    checked: AppSettings.hideExtensionsSidebar
                    hoverEnabled: true
                    onToggled: AppSettings.hideExtensionsSidebar = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: hideExtensionsSidebarSwitch.hovered
                    }
                }
                Button {
                    id: hideExtensionsSidebarResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("hideExtensionsSidebar")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("hideExtensionsSidebar")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: hideExtensionsSidebarResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: hideExtensionsSidebarResetButton.enabled && hideExtensionsSidebarResetButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Hide file extensions (main panel)")
                }
                Switch {
                    id: hideExtensionsMainPanelSwitch
                    checked: AppSettings.hideExtensionsMainPanel
                    hoverEnabled: true
                    onToggled: AppSettings.hideExtensionsMainPanel = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: hideExtensionsMainPanelSwitch.hovered
                    }
                }
                Button {
                    id: hideExtensionsMainPanelResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("hideExtensionsMainPanel")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("hideExtensionsMainPanel")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: hideExtensionsMainPanelResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: hideExtensionsMainPanelResetButton.enabled && hideExtensionsMainPanelResetButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("AI Auto-Labeling")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Auto-generate label, prelude, and tags")
                }
                Switch {
                    id: autoLabelEnabledSwitch
                    checked: AppSettings.autoLabelEnabled
                    hoverEnabled: true
                    onToggled: AppSettings.autoLabelEnabled = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: autoLabelEnabledSwitch.hovered
                    }
                }
                Button {
                    id: autoLabelEnabledResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("autoLabelEnabled")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("autoLabelEnabled")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: autoLabelEnabledResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: autoLabelEnabledResetButton.enabled && autoLabelEnabledResetButton.hovered
                    }
                }
            }

            Label {
                visible: autoLabelEnabledSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Runs a small local AI model (~2.5 GB download on first use) after each "
                           + "transcription to generate a display label, prelude, and tags.")
                color: "#888888"
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                visible: autoLabelEnabledSwitch.checked
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Only use existing tags (auto-categorize)")
                }
                Switch {
                    id: restrictTagsToExistingSwitch
                    checked: AppSettings.restrictTagsToExisting
                    hoverEnabled: true
                    onToggled: AppSettings.restrictTagsToExisting = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: restrictTagsToExistingSwitch.hovered
                    }
                }
                Button {
                    id: restrictTagsToExistingResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("restrictTagsToExisting")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("restrictTagsToExisting")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: restrictTagsToExistingResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: restrictTagsToExistingResetButton.enabled && restrictTagsToExistingResetButton.hovered
                    }
                }
            }

            Label {
                visible: autoLabelEnabledSwitch.checked && restrictTagsToExistingSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Off (default): freely invent new tags. On: only pick from tags already used "
                           + "elsewhere in this vault — leaves a file untagged rather than inventing one "
                           + "if nothing existing fits.")
                color: "#888888"
                font.pixelSize: 11
            }

            // Item 15 (Wave E): only meaningful when NOT hard-restricted —
            // restrictTagsToExisting above is already a hard rule, so this
            // gradient wouldn't have anything to adjust while it's on.
            RowLayout {
                Layout.fillWidth: true
                visible: autoLabelEnabledSwitch.checked && !restrictTagsToExistingSwitch.checked
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Tag reuse bias")
                }
                Slider {
                    id: tagReuseBiasSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 1
                    value: AppSettings.tagReuseBias
                    hoverEnabled: true
                    onMoved: AppSettings.tagReuseBias = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: tagReuseBiasSlider.hovered
                    }
                }
                Button {
                    id: tagReuseBiasResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("tagReuseBias")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("tagReuseBias")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: tagReuseBiasResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: tagReuseBiasResetButton.enabled && tagReuseBiasResetButton.hovered
                    }
                }
            }

            Label {
                visible: autoLabelEnabledSwitch.checked && !restrictTagsToExistingSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("How much to favor reusing an existing tag over inventing a new one. Low: "
                           + "prefer a precise new tag. High: stretch and reuse an existing tag even if "
                           + "it's only a loose fit.")
                color: "#888888"
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                visible: autoLabelEnabledSwitch.checked
                Label {
                    Layout.preferredWidth: 90
                    color: "white"
                    text: qsTr("Label wordiness")
                }
                Slider {
                    id: wordinessSlider
                    Layout.fillWidth: true
                    Layout.minimumWidth: 80
                    from: 0
                    to: 1
                    value: AppSettings.wordiness
                    hoverEnabled: true
                    onMoved: AppSettings.wordiness = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: wordinessSlider.hovered
                    }
                }
                Button {
                    id: wordinessResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("wordiness")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("wordiness")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: wordinessResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: wordinessResetButton.enabled && wordinessResetButton.hovered
                    }
                }
            }

            Label {
                visible: autoLabelEnabledSwitch.checked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("How verbose the AI-generated label and prelude are. Low: terse. High: "
                           + "descriptive.")
                color: "#888888"
                font.pixelSize: 11
            }

            Label {
                text: qsTr("Combine split recordings")
                color: "white"
                font.bold: true
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Some recorders split one continuous recording into several files (e.g. a "
                           + "size or time limit). Rules below auto-detect and merge those parts into "
                           + "one file — losslessly, before transcription — the instant a new part "
                           + "shows up in the vault.")
                color: "#888888"
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "white"
                    text: qsTr("Delete original parts after combining (instead of moving them to .combined)")
                }
                Switch {
                    id: deleteOriginalsAfterCombineSwitch
                    checked: AppSettings.deleteOriginalsAfterCombine
                    hoverEnabled: true
                    onToggled: AppSettings.deleteOriginalsAfterCombine = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: deleteOriginalsAfterCombineSwitch.hovered
                    }
                }
                Button {
                    id: deleteOriginalsAfterCombineResetButton
                    text: "↺"
                    implicitWidth: 22
                    implicitHeight: 22

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    enabled: !AppSettings.isDefault("deleteOriginalsAfterCombine")
                    opacity: enabled ? 1 : 0.35
                    hoverEnabled: enabled
                    onClicked: AppSettings.resetProperty("deleteOriginalsAfterCombine")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: deleteOriginalsAfterCombineResetButton.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: deleteOriginalsAfterCombineResetButton.enabled && deleteOriginalsAfterCombineResetButton.hovered
                    }
                }
            }

            Popup {
                id: regexHelpPopup
                width: 340
                modal: false
                focus: false
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                background: Rectangle {
                    color: "#2a2a2a"
                    radius: AppSettings.borderRadius
                    border.width: 1
                    border.color: "#3a3a3a"
                }

                contentItem: ColumnLayout {
                    spacing: 4
                    Label { text: qsTr("Regex quick reference"); color: "white"; font.bold: true }
                    Label { text: qsTr(".  any single character"); color: "#cccccc"; font.pixelSize: 11 }
                    Label { text: qsTr("*  +  ?  repeat: 0-or-more, 1-or-more, 0-or-1 times"); color: "#cccccc"; font.pixelSize: 11 }
                    Label { text: qsTr("^  $  start / end of the filename"); color: "#cccccc"; font.pixelSize: 11 }
                    Label { text: qsTr("\\d  \\w  \\s  a digit / word / space character"); color: "#cccccc"; font.pixelSize: 11 }
                    Label { text: qsTr("{n}  {n,m}  exactly n, or between n and m, repeats"); color: "#cccccc"; font.pixelSize: 11 }
                    Label { text: qsTr("[...]  any one of these characters"); color: "#cccccc"; font.pixelSize: 11 }
                    Label {
                        Layout.maximumWidth: 300
                        wrapMode: Text.WordWrap
                        color: "#cccccc"
                        font.pixelSize: 11
                        text: qsTr("(...)  capture group — text captured here must be IDENTICAL "
                                   + "across files for them to be combined. Everything else (literal "
                                   + "text, or an uncaptured wildcard) may vary freely between parts.")
                    }
                    Label {
                        Layout.maximumWidth: 300
                        wrapMode: Text.WordWrap
                        color: "#888888"
                        font.pixelSize: 11
                        text: qsTr("Example: ^(TX\\d{2})_MIC\\d{3}_\\d{8}_\\d{6}_orig\\.wav$ groups "
                                   + "files sharing the same TXnn id, even though the mic number and "
                                   + "timestamp differ between parts.")
                    }
                    Label {
                        Layout.maximumWidth: 300
                        wrapMode: Text.WordWrap
                        color: "#e8a33d"
                        font.pixelSize: 11
                        text: qsTr("|  (alternation) has the LOWEST precedence of any operator — "
                                   + "an ungrouped a|b$ means \"a\" OR \"b$\" as two whole separate "
                                   + "patterns. Always group it: (?:a|b).")
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Repeater {
                    model: AppSettings.combineRules

                    delegate: ColumnLayout {
                        id: ruleCard
                        required property var modelData
                        required property int index

                        property var previewGroups: []
                        property bool previewRun: false
                        property var differentiatorRuns: []

                        Layout.fillWidth: true
                        spacing: 4

                        // Inserts `snippet` at the current cursor position in
                        // rulePatternField, places the cursor `cursorOffset`
                        // characters into the inserted snippet, and refocuses
                        // the field so typing continues immediately. Uses
                        // TextInput.insert() rather than a raw `text =`
                        // reassignment — verified empirically (offscreen
                        // QTest) that reassigning the whole `text` property
                        // silently clears TextInput's undo history, while
                        // insert() records a normal undoable edit, so
                        // Ctrl+Z/Ctrl+Y keep working after clicking one of
                        // these buttons exactly as they do for typed input.
                        // Deliberately does NOT call settingsPopup.updateRule()
                        // itself — the field's own onEditingFinished commits
                        // the change on focus-out/Enter exactly as it does for
                        // manual typing, so insert-then-keep-typing and
                        // insert-then-click-away both commit the same way.
                        function insertPatternToken(snippet, cursorOffset) {
                            const pos = rulePatternField.cursorPosition
                            rulePatternField.insert(pos, snippet)
                            rulePatternField.cursorPosition = pos + cursorOffset
                            rulePatternField.forceActiveFocus()
                        }

                        // Re-runs the SAME detectCombineGroups() the ribbon
                        // "combine" button and per-job safety-net redirect
                        // use, so the preview below is always exactly what
                        // would really happen with the pattern as currently
                        // typed — not a separate simulation the user has to
                        // remember to re-trigger.
                        function updatePreview() {
                            if (rulePatternField.text.length > 0 && settingsPopup.vaultTreeModel
                                    && settingsPopup.transcriptionController) {
                                // Resolved against the LIVE typed text, not the
                                // committed rule.pattern — the pattern field can
                                // be mid-edit (uncommitted) here, so the sort
                                // differentiator's group index needs re-deriving
                                // the same way commitPatternChange() does on an
                                // actual commit, or a same-session edit that
                                // shifts group numbering would show a stale
                                // preview until the field loses focus.
                                const sortGroupIndex = settingsPopup.resolveSortGroupIndex(
                                            rulePatternField.text, ruleCard.modelData.sortGroupText || "",
                                            ruleCard.modelData.sortGroupOccurrence || 0)
                                const testRule = {name: ruleCard.modelData.name || qsTr("(unnamed)"),
                                                   pattern: rulePatternField.text, enabled: true,
                                                   sortGroupIndex: sortGroupIndex}
                                ruleCard.previewGroups = settingsPopup.transcriptionController.detectCombineGroups(
                                            [testRule], settingsPopup.vaultTreeModel.allAudioPaths())
                                ruleCard.previewRun = true
                            } else {
                                ruleCard.previewGroups = []
                                ruleCard.previewRun = false
                            }
                            ruleCard.updateDifferentiatorRuns()
                        }

                        // Recomputes the differentiator buttons for the
                        // pattern as currently typed, including — per run —
                        // which files (if any) would violate "this
                        // differentiator must participate in every match"
                        // (see TranscriptionController::
                        // differentiatorNonParticipatingFiles()'s doc
                        // comment: a zero-width capture is fine, only a
                        // capture that doesn't participate AT ALL, e.g. it
                        // sits in an alternation branch that wasn't taken,
                        // counts). For an OFF run this decides whether the
                        // button gets disabled (BLOCKED); for an already-ON
                        // run it decides whether a non-blocking warning is
                        // shown (nothing to block — the rule is already
                        // saved this way).
                        function updateDifferentiatorRuns() {
                            const text = rulePatternField.text
                            const runs = settingsPopup.findDifferentiatorRuns(text)
                            const hasBackend = settingsPopup.transcriptionController && settingsPopup.vaultTreeModel
                            const audioPaths = hasBackend ? settingsPopup.vaultTreeModel.allAudioPaths() : []
                            // Disambiguates duplicate-text ON runs (e.g. two
                            // "(\d+)" groups — an entirely realistic pattern,
                            // not a corner case) for the sort-differentiator
                            // designation: "the Nth ON run, left to right,
                            // with this exact text" — see
                            // resolveSortGroupIndex()'s doc comment, which
                            // this counting must stay in lockstep with (same
                            // isDifferentiator filter, same left-to-right
                            // order).
                            const occurrenceByText = {}
                            for (const run of runs) {
                                // groupIndex is the run's REAL (if already ON) or
                                // HYPOTHETICAL (if OFF, as if just toggled ON)
                                // capture-group number — cheap pure-text derivation,
                                // computed unconditionally so an ON button can color
                                // itself to match its differentiatorColor() preview
                                // span even when there's no backend/vault to run the
                                // participation check against.
                                const candidatePattern = run.isDifferentiator
                                        ? text : settingsPopup.toggleDifferentiatorRun(text, run)
                                run.groupIndex = settingsPopup.capturingGroupIndexAt(candidatePattern, run.start)
                                run.offendingFiles = (hasBackend && run.groupIndex > 0)
                                        ? settingsPopup.transcriptionController.differentiatorNonParticipatingFiles(
                                              candidatePattern, run.groupIndex, audioPaths)
                                        : []
                                if (run.isDifferentiator) {
                                    run.occurrence = occurrenceByText[run.text] || 0
                                    occurrenceByText[run.text] = run.occurrence + 1
                                } else {
                                    run.occurrence = 0
                                }
                            }
                            ruleCard.differentiatorRuns = runs
                        }

                        // Direct-commit toggle (like removeRuleButton /
                        // ruleEnabledSwitch above, NOT insertPatternToken's
                        // rely-on-the-field's-own-commit pattern) — this
                        // button's own click fully reassigns
                        // AppSettings.combineRules itself, synchronously,
                        // before any Repeater rebuild can happen, so it
                        // doesn't hit the focus-steal race described in the
                        // insert-token toolbar's doc comment below and
                        // doesn't need focusPolicy: Qt.NoFocus.
                        //
                        // Three-state cycle per run: OFF (plain text) ->
                        // GROUPING (capturing, contributes to the group
                        // identity key) -> SORT (still capturing, but
                        // EXCLUDED from the key — orders a group's members
                        // by this value instead, numeric-aware; see
                        // findCombineGroups()'s doc comment) -> back to OFF.
                        // Only one run per rule can be in the SORT state at
                        // a time — landing on it here always REPLACES
                        // whatever the rule's previous sort differentiator
                        // was, satisfying "a sort differentiator can't also
                        // be a grouping one" by construction rather than as
                        // a separate validation step.
                        function toggleDifferentiator(run) {
                            // Must compare occurrence too, not just text —
                            // two runs can share identical text (e.g. two
                            // "(\d+)" groups), and text alone can't tell
                            // which specific one is currently the sort field.
                            const isSortRun = run.isDifferentiator && ruleCard.modelData.sortGroupText === run.text
                                    && (ruleCard.modelData.sortGroupOccurrence || 0) === run.occurrence
                            if (isSortRun) {
                                // SORT -> OFF
                                const newPattern = settingsPopup.toggleDifferentiatorRun(rulePatternField.text, run)
                                rulePatternField.text = newPattern
                                settingsPopup.commitPatternChange(ruleCard.index, newPattern, "")
                            } else if (run.isDifferentiator) {
                                // GROUPING -> SORT (pattern text itself is
                                // unchanged — only the sort designation moves)
                                settingsPopup.commitPatternChange(ruleCard.index, rulePatternField.text, run.text, run.occurrence)
                            } else {
                                // OFF -> GROUPING
                                const newPattern = settingsPopup.toggleDifferentiatorRun(rulePatternField.text, run)
                                rulePatternField.text = newPattern
                                settingsPopup.commitPatternChange(ruleCard.index, newPattern)
                            }
                        }

                        // Debounced so the preview isn't recomputed on every
                        // single keystroke — coalesces into one scan shortly
                        // after typing settles, mirroring AppSettings' own
                        // scheduleSave() debounce pattern.
                        Timer {
                            id: previewTimer
                            interval: 300
                            onTriggered: ruleCard.updatePreview()
                        }

                        Component.onCompleted: ruleCard.updatePreview()

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: "#3a3a3a"
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            TextField {
                                id: ruleNameField
                                Layout.preferredWidth: 130
                                text: ruleCard.modelData.name
                                placeholderText: qsTr("Rule name")
                                hoverEnabled: true
                                background: UnderlineField { focused: ruleNameField.activeFocus }
                                onEditingFinished: settingsPopup.updateRule(ruleCard.index, "name", text)
                            }
                            Item { Layout.fillWidth: true }
                            Switch {
                                id: ruleEnabledSwitch
                                checked: ruleCard.modelData.enabled
                                hoverEnabled: true
                                onToggled: settingsPopup.updateRule(ruleCard.index, "enabled", checked)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: ruleEnabledSwitch.hovered }
                            }
                            Button {
                                id: removeRuleButton
                                text: qsTr("Remove")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: "#3a3a3a"
                                }
                                hoverEnabled: true
                                onClicked: settingsPopup.removeRule(ruleCard.index)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: removeRuleButton.hovered }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            TextField {
                                id: rulePatternField
                                Layout.fillWidth: true
                                text: ruleCard.modelData.pattern
                                placeholderText: qsTr("^(TX\\d{2})_MIC\\d{3}_.*_orig\\.wav$")
                                font.family: "monospace"
                                hoverEnabled: true
                                background: UnderlineField { focused: rulePatternField.activeFocus }
                                onEditingFinished: settingsPopup.commitPatternChange(ruleCard.index, text)
                                onTextChanged: previewTimer.restart()
                            }
                            Button {
                                id: regexInfoButton
                                text: "ⓘ"
                                implicitWidth: 22
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: "#3a3a3a"
                                }
                                hoverEnabled: true
                                onClicked: {
                                    regexHelpPopup.parent = regexInfoButton
                                    regexHelpPopup.x = -regexHelpPopup.width + regexInfoButton.width
                                    regexHelpPopup.y = regexInfoButton.height + 4
                                    regexHelpPopup.open()
                                }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: regexInfoButton.hovered }
                            }
                        }

                        // Live validity indicator — validates against the
                        // EXACT SAME QRegularExpression engine
                        // findCombineGroups()/matchRuleCapturedKey() actually
                        // use (TranscriptionController::regexValidationError()),
                        // not QML/JS RegExp, whose syntax isn't guaranteed
                        // identical. Reactive off rulePatternField.text — this
                        // is a plain Popup Repeater delegate, not a recycled
                        // TreeView row, so a keystroke-reactive binding is
                        // correct here (no explicit-push idiom needed).
                        Label {
                            id: patternValidityLabel
                            Layout.fillWidth: true
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            visible: rulePatternField.text.length > 0 && settingsPopup.transcriptionController
                            property string validationError: settingsPopup.transcriptionController
                                    ? settingsPopup.transcriptionController.regexValidationError(rulePatternField.text)
                                    : ""
                            color: validationError.length > 0 ? "#e0664f" : "#5fbf7a"
                            text: validationError.length > 0
                                    ? ("✗ " + validationError)
                                    : ("✓ " + qsTr("Valid"))
                            HoverHandler { id: patternValidityHover }
                            ToolTip.visible: patternValidityLabel.validationError.length > 0 && patternValidityHover.hovered
                            ToolTip.text: patternValidityLabel.validationError
                        }

                        // Insert-token toolbar: each button inserts a snippet
                        // at the cursor rather than committing directly, so
                        // typing/editing-finished behaves exactly as if the
                        // user had typed it themselves. "Either/Or" in
                        // particular exists so alternation is never typed
                        // ungrouped in the first place (see
                        // hasUngroupedAlternation()'s doc comment above).
                        // focusPolicy: Qt.NoFocus on every button below is
                        // required, not decorative: a click's press phase
                        // steals rulePatternField's active focus before the
                        // click's release phase fires onClicked, which fires
                        // rulePatternField's onEditingFinished, which commits
                        // via settingsPopup.updateRule() — that reassigns the
                        // WHOLE AppSettings.combineRules array, so the
                        // Repeater destroys and recreates every rule card
                        // (including the very button being pressed) before
                        // "clicked" can fire. Without NoFocus, insertion only
                        // succeeds on alternating clicks: the one after an
                        // insertion has divergent uncommitted text and steals
                        // focus, destroying the button mid-click; the next
                        // click (nothing to commit) succeeds normally.
                        Flow {
                            Layout.fillWidth: true
                            spacing: 4

                            Button {
                                id: insertDigitsButton
                                text: qsTr("Digits")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: "#3a3a3a"
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken("\\d+", 3)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertDigitsButton.hovered }
                            }
                            Button {
                                id: insertAnyButton
                                text: qsTr("Any")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: "#3a3a3a"
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken(".*?", 3)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertAnyButton.hovered }
                            }
                            Button {
                                id: insertGroupButton
                                text: qsTr("Group")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: "#3a3a3a"
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken("()", 1)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertGroupButton.hovered }
                            }
                            Button {
                                id: insertEitherOrButton
                                text: qsTr("Either/Or")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: "#3a3a3a"
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken("(?:|)", 3)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertEitherOrButton.hovered }
                            }
                            Button {
                                id: insertStartButton
                                text: qsTr("Start")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: "#3a3a3a"
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken("^", 1)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertStartButton.hovered }
                            }
                            Button {
                                id: insertEndButton
                                text: qsTr("End")
                                implicitHeight: 22
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: "#3a3a3a"
                                }
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                onClicked: ruleCard.insertPatternToken("$", 1)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: insertEndButton.hovered }
                            }
                        }

                        // Ungrouped-alternation footgun warning — the single
                        // highest-value guard here: `|` is the lowest-
                        // precedence regex operator, so an ungrouped one
                        // silently splits the WHOLE pattern into unrelated
                        // top-level alternatives instead of choosing between
                        // two local tokens. Only shown once the pattern is
                        // otherwise syntactically valid (an invalid pattern
                        // already gets its own error above).
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 11
                            color: "#e8a33d"
                            visible: rulePatternField.text.length > 0
                                     && patternValidityLabel.validationError.length === 0
                                     && settingsPopup.hasUngroupedAlternation(rulePatternField.text)
                            text: qsTr("This \"|\" isn't inside its own group, so it may match far more "
                                       + "than intended — e.g. \"^a_|b$\" really means \"start with a_\" OR "
                                       + "\"contains b$\" as two totally separate patterns, not \"a_ followed "
                                       + "by either b\". Wrap the alternatives: (?:a|b).")
                        }

                        // Rule-list-level overlap warning — non-blocking (see
                        // computeRuleOverlaps()'s doc comment: general regex
                        // disjointness is undecidable, this only catches
                        // identical-pattern-text and files that empirically
                        // match more than one enabled rule right now).
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 11
                            color: "#e8a33d"
                            property var overlaps: settingsPopup.combineOverlapWarnings[ruleCard.modelData.name] || []
                            visible: overlaps.length > 0
                            text: {
                                if (overlaps.length === 0)
                                    return ""
                                const withFile = overlaps.filter(function (o) { return o.file.length > 0 })
                                const identical = overlaps.filter(function (o) { return o.file.length === 0 })
                                const parts = []
                                if (identical.length > 0)
                                    parts.push(qsTr("Same pattern as: %1").arg(
                                            identical.map(function (o) { return o.other }).join(", ")))
                                if (withFile.length > 0) {
                                    const shown = withFile.slice(0, 3).map(function (o) {
                                        return o.other + " (" + o.file + ")"
                                    }).join(", ")
                                    const extra = withFile.length > 3 ? qsTr(" +%1 more").arg(withFile.length - 3) : ""
                                    parts.push(qsTr("Also matches the same file(s) as: %1%2").arg(shown).arg(extra))
                                }
                                return "⚠ " + parts.join(" ")
                            }
                        }

                        // Differentiator buttons: one per non-exact-match
                        // ("special") run of the pattern as currently typed
                        // (see findDifferentiatorRuns()'s doc comment above).
                        // Clicking toggles that run between a plain/
                        // non-capturing match (still required for the file
                        // to be recognized as part of a sequential group,
                        // but doesn't affect WHICH group) and a real
                        // capturing group (its captured text must be
                        // identical across files for them to combine — see
                        // matchRuleCapturedKey() in
                        // TranscriptionController.cpp). Direct-commit click
                        // handler (see ruleCard.toggleDifferentiator()'s doc
                        // comment) — no focusPolicy: Qt.NoFocus needed here,
                        // unlike the insert-token toolbar below.
                        Flow {
                            Layout.fillWidth: true
                            spacing: 4
                            visible: ruleCard.differentiatorRuns.length > 0

                            Repeater {
                                model: ruleCard.differentiatorRuns
                                delegate: Button {
                                    id: differentiatorButton
                                    required property var modelData
                                    readonly property bool blocked: !modelData.isDifferentiator
                                            && modelData.offendingFiles.length > 0
                                    // The third state of the OFF -> GROUPING ->
                                    // SORT -> OFF cycle (see toggleDifferentiator()'s
                                    // doc comment) — this run currently orders its
                                    // group's files instead of identifying the
                                    // group, so it deliberately does NOT get a
                                    // differentiatorColor() (that palette means
                                    // "same color = same group," which no longer
                                    // applies once a run stops contributing to the
                                    // key).
                                    // Occurrence, not just text, must match —
                                    // two runs can share identical text (e.g.
                                    // two "(\d+)" groups), and only ONE of
                                    // them is really the sort field.
                                    readonly property bool isSortRun: ruleCard.modelData.sortGroupText === modelData.text
                                            && (ruleCard.modelData.sortGroupOccurrence || 0) === modelData.occurrence
                                    // The button's own highlight uses the SAME
                                    // per-group color classifyFilenameHtml() paints
                                    // that differentiator's text with in the preview
                                    // below — pairs each color to the regex snippet
                                    // that produced it (see differentiatorPalette's
                                    // doc comment: this pairing is the accessibility
                                    // mitigation for the palette's CVD floor-band
                                    // warning, not just a nice-to-have). Qt.darker()/
                                    // Qt.lighter() derive the fill/hover states from
                                    // that one base color — the only place this file
                                    // computes color variants instead of hand-picking
                                    // hex per state, because 8 palette slots x 3
                                    // states each isn't worth hand-tuning.
                                    readonly property color baseColor: differentiatorButton.isSortRun
                                            ? "#888888" : settingsPopup.differentiatorColor(modelData.groupIndex)
                                    text: (differentiatorButton.isSortRun ? "⇅ " : (modelData.isDifferentiator ? "✓ " : "")) + modelData.text
                                    implicitHeight: 22
                                    enabled: !blocked
                                    background: Rectangle {
                                        radius: AppSettings.borderRadius
                                        color: differentiatorButton.modelData.isDifferentiator
                                                ? Qt.darker(differentiatorButton.baseColor, differentiatorButton.hovered ? 1.7 : 2.4)
                                                : (differentiatorButton.hovered && !differentiatorButton.blocked
                                                   ? "#3a3a3a" : "#2a2a2a")
                                        border.width: 1
                                        border.color: differentiatorButton.modelData.isDifferentiator
                                                ? differentiatorButton.baseColor : "#3a3a3a"
                                        opacity: differentiatorButton.blocked ? 0.5 : 1
                                    }
                                    font.family: "monospace"
                                    font.pixelSize: 11
                                    hoverEnabled: true
                                    onClicked: ruleCard.toggleDifferentiator(modelData)
                                    HoverHandler {
                                        id: differentiatorHover
                                        cursorShape: differentiatorButton.blocked ? Qt.ArrowCursor : Qt.PointingHandCursor
                                    }
                                    HoverBorder { anchors.fill: parent; active: differentiatorHover.hovered && !differentiatorButton.blocked }
                                    ToolTip.visible: differentiatorHover.hovered
                                    ToolTip.text: {
                                        if (differentiatorButton.blocked) {
                                            const shown = modelData.offendingFiles.slice(0, 3).join(", ")
                                            const extra = modelData.offendingFiles.length > 3
                                                    ? qsTr(" (+%1 more)").arg(modelData.offendingFiles.length - 3) : ""
                                            return qsTr("Can't make this a differentiator — it doesn't appear in "
                                                        + "some matching files at all: %1%2").arg(shown).arg(extra)
                                        }
                                        if (modelData.isDifferentiator && modelData.offendingFiles.length > 0) {
                                            const shown = modelData.offendingFiles.slice(0, 3).join(", ")
                                            return qsTr("Warning: doesn't appear in some matching files: %1").arg(shown)
                                        }
                                        if (differentiatorButton.isSortRun)
                                            return qsTr("Sorts combined files by this value (numeric if every "
                                                        + "matched file has a numeric one here, otherwise "
                                                        + "alphabetical) instead of grouping by it. Click to turn off.")
                                        if (modelData.isDifferentiator)
                                            return qsTr("Differentiator — must match identically for files to "
                                                        + "combine. Click to sort combined files by this instead.")
                                        return qsTr("Click to make this a differentiator")
                                    }
                                }
                            }
                        }

                        // Non-blocking warning for an already-ON
                        // differentiator that doesn't participate in every
                        // currently-matched file — can't block a click that
                        // already happened / a rule that's already saved,
                        // so this is advisory only (see
                        // updateDifferentiatorRuns()'s doc comment).
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 11
                            color: "#e8a33d"
                            property var brokenOnRuns: ruleCard.differentiatorRuns.filter(function (r) {
                                return r.isDifferentiator && r.offendingFiles.length > 0
                            })
                            visible: brokenOnRuns.length > 0
                            text: brokenOnRuns.length > 0
                                    ? qsTr("⚠ Differentiator \"%1\" doesn't appear in some matching files: %2")
                                          .arg(brokenOnRuns[0].text)
                                          .arg(brokenOnRuns[0].offendingFiles.slice(0, 3).join(", "))
                                    : ""
                        }

                        // Live syntax-highlight readout — reflects every
                        // keystroke in rulePatternField directly (not the
                        // committed modelData.pattern), same "see it as you
                        // type" intent as the regex info popup above.
                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.RichText
                            wrapMode: Text.Wrap
                            font.family: "monospace"
                            font.pixelSize: 11
                            text: settingsPopup.highlightPattern(rulePatternField.text)
                            visible: rulePatternField.text.length > 0
                        }

                        Label {
                            visible: ruleCard.previewRun && ruleCard.previewGroups.length === 0
                            text: qsTr("No files in this vault match.")
                            color: "#888888"
                            font.pixelSize: 11
                        }

                        // Preview cards: one per detected group, showing the
                        // combine order and what would happen — this is the
                        // SAME detectCombineGroups() the ribbon "combine"
                        // button and the per-job safety-net redirect both
                        // use, so what's shown here is exactly what would
                        // really happen, not a separate simulation.
                        Repeater {
                            model: ruleCard.previewRun ? ruleCard.previewGroups : []
                            delegate: Rectangle {
                                id: previewCard
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.preferredHeight: previewCardColumn.implicitHeight + 12
                                radius: AppSettings.borderRadius
                                color: "#1e1e1e"
                                border.width: 1
                                border.color: "#3a3a3a"

                                ColumnLayout {
                                    id: previewCardColumn
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    spacing: 2
                                    // Header: exactly which differentiator
                                    // values grouped these files together —
                                    // color-matched to the differentiator
                                    // toggle buttons and the per-filename
                                    // highlighting below, so "same color
                                    // here as up there" tells you why these
                                    // files landed in the same group without
                                    // reading regex.
                                    TextEdit {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        readOnly: true
                                        selectByMouse: true
                                        selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)
                                        color: "#888888"
                                        font.pixelSize: 10
                                        font.bold: true
                                        textFormat: Text.RichText
                                        text: {
                                            const values = previewCard.modelData.capturedValues || []
                                            if (values.length === 0)
                                                return qsTr("No differentiators — this rule groups every matching file together")
                                            return qsTr("Differentiators: ") + values.map(function (v, i) {
                                                const shown = v.length > 0 ? settingsPopup.escapeRegexPreviewHtml(v) : qsTr("(empty)")
                                                return "<span style=\"color:" + settingsPopup.differentiatorColor(i + 1) + ";\">"
                                                        + shown + "</span>"
                                            }).join(", ")
                                        }
                                    }
                                    TextEdit {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        readOnly: true
                                        selectByMouse: true
                                        selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)
                                        color: "#dddddd"
                                        font.pixelSize: 11
                                        text: {
                                            switch (previewCard.modelData.status) {
                                            case "ready": return qsTr("Ready to combine (%1 parts):").arg(previewCard.modelData.files.length)
                                            case "singleton": return qsTr("Only one matching file — nothing to combine yet.")
                                            case "alreadyTranscribed": return qsTr("Skipped — one of these parts is already transcribed.")
                                            case "alreadyCombined": return qsTr("Already combined into %1.").arg(previewCard.modelData.outputPath)
                                            case "pending": return qsTr("Waiting — the most recent part was modified very recently.")
                                            default: return previewCard.modelData.status
                                            }
                                        }
                                    }
                                    TextEdit {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        visible: previewCard.modelData.files.length > 0
                                        readOnly: true
                                        selectByMouse: true
                                        selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)
                                        color: "#888888"
                                        font.pixelSize: 10
                                        textFormat: Text.RichText
                                        text: previewCard.modelData.files.map(function (f, i) {
                                            const name = f.substring(f.lastIndexOf("/") + 1)
                                            return (i + 1) + ". " + settingsPopup.classifyFilenameHtml(rulePatternField.text, name)
                                        }).join("<br>")
                                    }
                                    TextEdit {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        visible: previewCard.modelData.status === "ready"
                                        readOnly: true
                                        selectByMouse: true
                                        selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)
                                        color: "#888888"
                                        font.pixelSize: 10
                                        text: qsTr("Output: %1").arg(
                                                  previewCard.modelData.outputPath.substring(
                                                      previewCard.modelData.outputPath.lastIndexOf("/") + 1))
                                    }
                                }
                            }
                        }
                    }
                }

                Button {
                    id: addRuleButton
                    text: qsTr("+ Add rule")
                    Layout.alignment: Qt.AlignLeft
                    implicitHeight: 24
                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (addRuleButton.hovered ? "#3a3a3a" : "#2a2a2a")
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    hoverEnabled: true
                    onClicked: settingsPopup.addRule()
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    HoverBorder { anchors.fill: parent; active: addRuleButton.hovered }
                }
            }
        }
    }

    // Chrome/Firefox-style middle-click autoscroll for the Settings
    // ScrollView — same two-gesture pattern (quick click arms free-scroll;
    // press-drag-release disarms on release) as MainPanel's transcript-view
    // and Sidebar's file-tree middleDragScrollArea. Lives as a sibling
    // overlay of scrollArea (not inside it) so it isn't counted as the
    // ScrollView's own content; anchors match scrollArea's own margins so
    // it only covers the actual visible scroll region, not the popup's
    // title bar / rule-editor buttons that sit outside it.
    //
    // NOT a MouseArea, and NOT z-toggled — both were tried and both broke
    // something real, confirmed empirically each time (offscreen QTest):
    //   1. A statically-topmost MouseArea (the first version) reliably
    //      caught the click but stole the cursor from every hoverable
    //      button/slider underneath, even fully idle — Qt Quick's cursor
    //      resolution is pure top-down paint-order for the topmost HIT
    //      item, independent of hoverEnabled/acceptedButtons, and MouseArea
    //      always implicitly claims a cursor (default ArrowCursor) merely
    //      by being the topmost hit item, regardless of its own config.
    //   2. Toggling MouseArea's z below (-1) while idle (the second
    //      version, matching Sidebar's TreeView-based fix) fixed the cursor
    //      but broke the click itself: unlike a bare Flickable (which only
    //      claims LeftButton by default and lets MiddleButton fall
    //      through), QQC2's ScrollView/T.ScrollView Control claims ALL
    //      mouse-button presses within its bounds at the outer Control
    //      level before any per-button filtering, so a lower-z sibling
    //      never sees a middle-click over it at all. Sidebar's tree isn't
    //      wrapped in a Control the same way, which is why that fix never
    //      needed to contend with this.
    // The actual fix: a plain Item (not MouseArea) has NO implicit cursor
    // at all, so it can sit PERMANENTLY topmost without poisoning cursor
    // resolution for anything underneath, AS LONG AS it never itself binds
    // an ENABLED cursor-setting handler while idle (confirmed empirically:
    // a disabled HoverHandler{cursorShape:...} — enabled only once armed —
    // doesn't count either). A permanently-topmost TapHandler reliably
    // catches the middle-button press through ScrollView's swallow (a
    // TapHandler participates in per-button hit-testing the same way
    // MouseArea's fallthrough does, but being an Item-not-MouseArea it
    // doesn't drag the implicit-cursor baggage along). gesturePolicy:
    // DragWithinBounds (not the default DragThreshold) matters too: the
    // default cancels TapHandler's own grab — and stops delivering ANY
    // further events to anyone, including the real release — the instant
    // the drag exceeds the platform threshold, which is far too early for
    // gesture 2's long hold-drag; DragWithinBounds only cancels if the
    // pointer leaves this Item's bounds, which for a full-viewport overlay
    // essentially never happens mid-gesture.
    Item {
        id: middleDragScrollArea
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.topMargin: 16
        anchors.bottomMargin: 16
        anchors.rightMargin: 2
        z: 1

        property bool armed: false
        property real originY: 0
        property real currentY: 0
        property bool draggedWhileHeld: false
        readonly property int deadzone: 8

        // Drives arming (both gestures start here) and gesture 2's
        // continued tracking while the button stays held — point.position
        // updates live during the whole held drag under DragWithinBounds,
        // unlike the default gesturePolicy which would stop updating (and
        // stop being told about the eventual release) as soon as the drag
        // threshold is crossed.
        TapHandler {
            id: middleDragTap
            acceptedButtons: middleDragScrollArea.armed
                             ? (Qt.LeftButton | Qt.MiddleButton | Qt.RightButton)
                             : Qt.MiddleButton
            gesturePolicy: TapHandler.DragWithinBounds
            onPressedChanged: {
                if (middleDragTap.pressed) {
                    // Any click while already armed cancels it (Chrome
                    // convention) instead of performing that button's
                    // normal action — acceptedButtons above ensures this
                    // branch is only reachable for non-middle buttons once
                    // armed.
                    if (middleDragScrollArea.armed) {
                        middleDragScrollArea.armed = false
                        return
                    }
                    middleDragScrollArea.draggedWhileHeld = false
                    middleDragScrollArea.armed = true
                    middleDragScrollArea.originY = middleDragTap.point.position.y
                    middleDragScrollArea.currentY = middleDragTap.point.position.y
                } else if (middleDragScrollArea.draggedWhileHeld) {
                    middleDragScrollArea.armed = false
                    // else: real click, no drag — gesture 1 keeps running.
                }
            }
            onPointChanged: {
                if (!middleDragTap.pressed)
                    return
                middleDragScrollArea.currentY = middleDragTap.point.position.y
                if (!middleDragScrollArea.draggedWhileHeld
                        && Math.abs(middleDragScrollArea.currentY - middleDragScrollArea.originY) > middleDragScrollArea.deadzone) {
                    middleDragScrollArea.draggedWhileHeld = true
                }
            }
        }

        // Drives gesture 1's free-scroll (no button held) and the visible
        // cursor — enabled only while armed, so it never contributes a
        // cursor (and never blocks hover-event delivery to covered
        // buttons) while idle.
        HoverHandler {
            id: middleDragHover
            enabled: middleDragScrollArea.armed
            cursorShape: {
                const dy = Math.abs(middleDragScrollArea.currentY - middleDragScrollArea.originY)
                return dy <= middleDragScrollArea.deadzone ? Qt.SizeAllCursor : Qt.SizeVerCursor
            }
            onPointChanged: {
                if (middleDragScrollArea.armed && !middleDragTap.pressed)
                    middleDragScrollArea.currentY = middleDragHover.point.position.y
            }
        }

        Timer {
            interval: 16
            repeat: true
            running: middleDragScrollArea.armed
            onTriggered: {
                const flick = scrollArea.contentItem
                const dy = middleDragScrollArea.currentY - middleDragScrollArea.originY
                const deadzone = middleDragScrollArea.deadzone
                if (Math.abs(dy) <= deadzone)
                    return
                const beyond = dy - Math.sign(dy) * deadzone
                const speed = Math.sign(beyond) * beyond * beyond * 0.002
                const maxY = Math.max(0, flick.contentHeight - flick.height)
                flick.contentY = Math.max(0, Math.min(maxY, flick.contentY + speed))
            }
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: middleDragScrollArea.armed
        onActivated: middleDragScrollArea.armed = false
    }
}
