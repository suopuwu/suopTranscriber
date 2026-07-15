#include <QtTest>

#include <QDir>
#include <QFile>
#include <QRegularExpression>

// Source-scan checks for the conventions in StyleGuide.md and the environment
// constraints in CLAUDE.md ("force QT_MEDIA_BACKEND=ffmpeg"). These read the
// checked-in sources (paths injected by tests/CMakeLists.txt), so they guard
// the conventions without needing a running GUI.
class TstStyleGuide : public QObject
{
    Q_OBJECT

    static QString readAll(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        return QString::fromUtf8(f.readAll());
    }

private slots:
    // Regression: Sidebar.qml's treeDelegate (a TreeView row) recycles a
    // fixed pool of delegate Items as rows scroll in/out of view — required
    // properties like path/isAudio get reassigned to the recycled Item for
    // its new row, but metadata/jobStatus are plain imperative properties
    // only ever pushed at Component.onCompleted. Without an onPathChanged
    // refresh, a recycled Item kept showing whichever label/job-status it
    // FIRST loaded, forever, regardless of which file it was recycled to
    // represent — this made one label appear to "repeat" throughout the
    // whole tree at a fixed interval (the delegate pool size). No QQuickView
    // in this test suite to actually drive TreeView recycling, so this is a
    // source-scan guard against the fix regressing rather than a behavioral
    // test.
    void treeDelegate_refreshesMetadataAndJobStatusOnRecycle()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");
        const int idx = src.indexOf(QStringLiteral("onPathChanged"));
        QVERIFY2(idx >= 0,
                 "treeDelegate must refresh metadata/jobStatus onPathChanged — TreeView recycles "
                 "delegate Items across rows, so a Component.onCompleted-only push goes stale on reuse");
        const QString after = src.mid(idx, 300);
        QVERIFY2(after.contains(QStringLiteral("refreshMetadata()")),
                 "onPathChanged must call refreshMetadata() so recycled rows show the right label");
        QVERIFY2(after.contains(QStringLiteral("refreshJobStatus()")),
                 "onPathChanged must call refreshJobStatus() so recycled rows show the right job status");
    }

    // Regression: treeDelegate's filename Text had no width bound at all, so
    // a long filename (routine for split-recording combine candidates — the
    // very files Item 51's groupMergeButton targets) painted straight
    // through the per-row icon cluster on the right (rowTranscribeButton/
    // rowRelabelButton/rowChaptersButton/groupMergeButton) instead of
    // stopping short of it, so the "⊕" combine button rendered on top of
    // mid-filename characters instead of in its own clear space — reading as
    // "no combine icon" rather than a garbled one. No QQuickView in this
    // test suite to actually lay out a long filename and inspect the
    // rendered pixels, so this is a source-scan guard against the fix
    // (bounded width + elide + clip) regressing rather than a behavioral
    // test.
    //
    // Item 8 follow-up bug: elide was unconditional, but this same Text
    // switches textFormat to Text.StyledText whenever a search query is
    // active (for the highlight <span> markup) — elide+StyledText together
    // is documented elsewhere in this file (snippetText, item 2/Wave E) as
    // unsupported/unpredictable in Qt Quick's Text, and broke search-result
    // filename spacing. elide must now be OFF exactly when textFormat is
    // StyledText, with clip: true as the backstop that still keeps overflow
    // from painting over the icon cluster while elide is off.
    void treeDelegate_filenameTextReservesSpaceForRowIconCluster()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");
        const int rowIdx = src.indexOf(QStringLiteral("id: rowTranscribeButton"));
        QVERIFY2(rowIdx >= 0, "rowTranscribeButton must exist in Sidebar.qml's treeDelegate");
        const QString before = src.left(rowIdx);
        const int textIdx = before.indexOf(QStringLiteral("useLabelsSidebar"));
        QVERIFY2(textIdx >= 0, "treeDelegate's filename Text (AppSettings.useLabelsSidebar) must exist "
                               "before the per-row icon cluster");
        const QString filenameTextBlock = before.mid(textIdx, before.length() - textIdx);
        QVERIFY2(filenameTextBlock.contains(QStringLiteral("Text.ElideRight"))
                     && filenameTextBlock.contains(QStringLiteral("Text.ElideNone")),
                 "treeDelegate's filename Text must elide when plain text (otherwise a long filename "
                 "overlaps the per-row icon cluster) but must be able to go elide-less too (needed "
                 "whenever textFormat is StyledText — see below)");
        QVERIFY2(filenameTextBlock.contains(QStringLiteral("elide: textFormat === Text.PlainText "
                                                            "? Text.ElideRight : Text.ElideNone")),
                 "elide must be conditional on textFormat, OFF exactly when textFormat is StyledText — "
                 "elide+StyledText together is unsupported/unpredictable in Qt Quick's Text (item 8 bug: "
                 "unconditional elide broke search-result filename spacing once textFormat switched to "
                 "StyledText for the highlight markup)");
        QVERIFY2(filenameTextBlock.contains(QStringLiteral("clip: true")),
                 "treeDelegate's filename Text must clip — the backstop that keeps overflow from "
                 "painting over the icon cluster even while elide is conditionally off");
    }

    // StyleGuide.md: the hover outline is the reusable HoverBorder.qml — a
    // transparent Rectangle, 1px white border, radius from AppSettings,
    // opacity animated in on hover.
    void hoverBorder_componentContract()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/HoverBorder.qml"));
        QVERIFY2(!src.isEmpty(), "qml/HoverBorder.qml must exist (StyleGuide.md hover convention)");
        QVERIFY(src.contains(QStringLiteral("color: \"transparent\"")));
        QVERIFY(src.contains(QStringLiteral("radius: AppSettings.borderRadius")));
        QVERIFY(src.contains(QStringLiteral("border.width: 1")));
        QVERIFY2(src.contains(QStringLiteral("border.color: AppSettings.accentColor")),
                 "hover outline color must come from the configurable AppSettings.accentColor, not a "
                 "hardcoded \"white\"");
        QVERIFY2(src.contains(QStringLiteral("Behavior on opacity")),
                 "hover outline must fade, not pop (StyleGuide.md)");
    }

    // Always-editable fields with no other visual cue that they're a text
    // box (title/tags/speakers/prelude) should hint at their editability on
    // hover — the underline previews at 50% opacity before the field is
    // actually focused (1). Source-scan since there's no QQuickView here to
    // drive a real HoverHandler.
    void underlineField_hoverPreviewsAtHalfOpacity()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/UnderlineField.qml"));
        QVERIFY2(!src.isEmpty(), "qml/UnderlineField.qml must exist");
        QVERIFY2(src.contains(QStringLiteral("HoverHandler")),
                 "UnderlineField must track hover state to preview the underline before focus");
        QVERIFY2(src.contains(QStringLiteral("hovered ? 0.5 : 0")),
                 "unfocused hover state must show the underline at 50% opacity, not fully hidden or "
                 "fully shown");
    }

    // StyleGuide.md "Always-editable text": display-vs-edit must never be a
    // swap between two different elements gated by a focus-mode bool — that
    // pattern is exactly what let the native right-click "Copy" context
    // menu's focus-steal hide the edit field out from under the user
    // (titleField's old renamingTitle/commitRename() bug). Guards against
    // the swap creeping back in for any of the five always-editable fields;
    // no QQuickView harness in this suite to exercise the actual right-click
    // behavior directly.
    void mainPanel_hasNoDisplayEditSwapStateForAlwaysEditableFields()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");
        QVERIFY2(!src.contains(QStringLiteral("renamingTitle")),
                 "titleField must be always-editable, not swapped via a renamingTitle bool");
        QVERIFY2(!src.contains(QStringLiteral("editingPrelude")),
                 "preludeArea must be always-editable, not swapped via an editingPrelude bool");
        QVERIFY2(!src.contains(QStringLiteral("renamingSpeakerId")),
                 "speakerNameField must be always-editable, not swapped via a renamingSpeakerId property");
        QVERIFY2(src.contains(QStringLiteral("id: titleField")) && src.contains(QStringLiteral("id: preludeArea"))
                     && src.contains(QStringLiteral("id: speakerNameField")) && src.contains(QStringLiteral("id: tagField"))
                     && src.contains(QStringLiteral("id: tagAddField")) && src.contains(QStringLiteral("id: transcriptArea")),
                 "all always-editable fields (title/prelude/speaker/tag/tag-add/transcript) must exist");

        // transcriptArea must not be unconditionally readOnly (the old
        // always-read-only TextArea, before segment text became editable).
        const int transcriptAreaIdx = src.indexOf(QStringLiteral("id: transcriptArea"));
        QVERIFY(transcriptAreaIdx >= 0);
        const QString transcriptAreaBlock = src.mid(transcriptAreaIdx, 600);
        QVERIFY2(!transcriptAreaBlock.contains(QStringLiteral("readOnly: true")),
                 "transcriptArea must be conditionally editable, not hardcoded readOnly: true");
    }

    // StyleGuide.md: one global AppSettings.borderRadius, used everywhere a
    // corner is rounded — never an ad-hoc literal. Exception: a radius equal
    // to half the item's width/height is drawing a circle (status dots,
    // slider handle), not rounding a corner.
    void borderRadius_noHardcodedCornerRadii()
    {
        const QDir qmlDir(QStringLiteral(SUOP_QML_DIR));
        const QRegularExpression radiusLiteral(QStringLiteral("^\\s*radius:\\s*(\\d+(?:\\.\\d+)?)\\s*$"));
        QStringList violations;
        const QStringList files = qmlDir.entryList({QStringLiteral("*.qml")}, QDir::Files);
        QVERIFY(!files.isEmpty());
        for (const QString &name : files) {
            const QStringList lines = readAll(qmlDir.filePath(name)).split(QLatin1Char('\n'));
            for (int i = 0; i < lines.size(); ++i) {
                const auto m = radiusLiteral.match(lines[i]);
                if (!m.hasMatch())
                    continue;
                const double r = m.captured(1).toDouble();
                // Circle idiom: nearby width/height == 2 * radius.
                bool isCircle = false;
                const QRegularExpression size(
                    QStringLiteral("^\\s*(?:width|height|implicitWidth|implicitHeight|"
                                   "Layout\\.preferredWidth|Layout\\.preferredHeight):"
                                   "\\s*(\\d+(?:\\.\\d+)?)\\s*$"));
                for (int j = qMax(0, i - 6); j < qMin<qsizetype>(lines.size(), i + 7); ++j) {
                    const auto s = size.match(lines[j]);
                    if (s.hasMatch() && qFuzzyCompare(s.captured(1).toDouble(), 2.0 * r))
                        isCircle = true;
                }
                if (!isCircle)
                    violations << QStringLiteral("%1:%2: %3").arg(name).arg(i + 1).arg(lines[i].trimmed());
            }
        }
        QVERIFY2(violations.isEmpty(),
                 qPrintable(QStringLiteral("hardcoded corner radius (use AppSettings.borderRadius):\n")
                            + violations.join(QLatin1Char('\n'))));
    }

    // StyleGuide.md: settings are properties on the AppSettings singleton
    // (no bespoke storage), edited via SettingsPopup bound to it, with the
    // defaults map as the single source of truth for reset buttons.
    void settings_appSettingsSingletonConvention()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/AppSettings.qml"));
        QVERIFY(src.startsWith(QStringLiteral("pragma Singleton")));
        QVERIFY2(src.contains(QStringLiteral("import QtCore")) && src.contains(QStringLiteral("Settings {")),
                 "AppSettings persists via QtCore Settings (QSettings-backed), no bespoke storage");
        QVERIFY(src.contains(QStringLiteral("readonly property var defaults:")));
        QVERIFY(src.contains(QStringLiteral("function isDefault(")));
        QVERIFY(src.contains(QStringLiteral("function resetProperty(")));
        QVERIFY(src.contains(QStringLiteral("function resetAll(")));

        const QString popup = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(popup.contains(QStringLiteral("AppSettings.")),
                 "settings UI lives in SettingsPopup.qml bound to AppSettings (StyleGuide.md)");
    }

    // Combine-rule regex authoring aids, added to prevent the "ungrouped
    // alternation" footgun (e.g. `^TX0._MIC*._(edit)|(orig)`, which really
    // parses as two fully independent top-level alternatives because `|` has
    // the lowest precedence of any regex operator). No QQuickView in this
    // suite to drive the live bindings, so this is a source-scan guard that
    // the validity indicator, the ungrouped-alternation warning, and the
    // insert-token toolbar all still exist, rather than a behavioral test.
    void settingsPopup_combineRulePatternAuthoringAids()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(!src.isEmpty(), "qml/SettingsPopup.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("transcriptionController.regexValidationError(")),
                 "the pattern field's live validity indicator must call "
                 "TranscriptionController::regexValidationError() (the real QRegularExpression engine), "
                 "not a QML/JS RegExp re-implementation");

        QVERIFY2(src.contains(QStringLiteral("function hasUngroupedAlternation(")),
                 "must scan for a top-level (ungrouped) alternation — the exact bug in "
                 "^TX0._MIC*._(edit)|(orig), where the un-anchored `(orig)` alternative "
                 "matches that substring anywhere in any filename");
        QVERIFY2(src.contains(QStringLiteral("hasUngroupedAlternation(rulePatternField.text)")),
                 "the warning must be reactive off the live pattern field text, not just the committed rule");
        QVERIFY2(src.contains(QStringLiteral("Wrap the alternatives: (?:a|b)")),
                 "the inline warning must show the concrete fix, not just flag the problem");

        QVERIFY2(src.contains(QStringLiteral("insertPatternToken(")),
                 "an insert-token toolbar must exist so alternation/groups can be typed correctly "
                 "without hand-authoring raw regex syntax");
        QVERIFY2(src.contains(QStringLiteral("insertPatternToken(\"(?:|)\"")),
                 "the Either/Or token must insert an already-grouped (?:|) — the whole point is "
                 "that alternation is never inserted ungrouped in the first place");
    }

    // Combine-rule "differentiator" buttons: every non-exact-match run of a
    // rule's pattern gets a clickable button that toggles whether files must
    // share that run's captured text to combine. No QQuickView in this
    // suite to drive the live bindings, so this is a source-scan guard that
    // the toggle mechanism, the participation-block check, and the
    // rule-overlap warning all still exist, rather than a behavioral test.
    void settingsPopup_combineRuleDifferentiatorButtons()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(!src.isEmpty(), "qml/SettingsPopup.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("function findDifferentiatorRuns(")),
                 "must derive the clickable runs from the pattern's non-exact-match (\"special\") "
                 "tokens, merging consecutive ones together");
        QVERIFY2(src.contains(QStringLiteral("function toggleDifferentiatorRun(")),
                 "must mutate the rule's pattern text to add/remove a capturing group — "
                 "matchRuleCapturedKey() (TranscriptionController.cpp) already treats every "
                 "capture group as differentiating, so no C++ changes are needed for the toggle itself");
        QVERIFY2(src.contains(QStringLiteral("function capturingGroupIndexAt(")),
                 "capture-group numbering must be derived from a real paren-scan, not the "
                 "literal/special display classifier — a literal-content group like (orig) is "
                 "still a real capturing group to QRegularExpression");

        QVERIFY2(src.contains(QStringLiteral("transcriptionController.differentiatorNonParticipatingFiles(")),
                 "the block/warning check must call the real QRegularExpression engine "
                 "(TranscriptionController::differentiatorNonParticipatingFiles()), not a QML/JS "
                 "RegExp re-implementation, since it hinges on capture-group participation semantics");
        QVERIFY2(src.contains(QStringLiteral("enabled: !blocked")),
                 "a run whose differentiator wouldn't participate in every currently-matched file "
                 "must have its toggle button disabled (blocked), not just warned about");

        QVERIFY2(src.contains(QStringLiteral("function computeRuleOverlaps(")),
                 "must warn (non-blocking — regex disjointness is undecidable in general) when two "
                 "enabled rules empirically match the same vault file, or have identical pattern text");
    }

    // Each differentiator (real capturing group) must render in a distinct
    // color in the combine-rule preview, and its button must match — so a
    // user can visually confirm "same color everywhere -> these files
    // combine" without reading regex. No QQuickView in this suite, so this
    // is a source-scan guard that the palette, the preview overlay, and the
    // button/preview color pairing all still exist.
    void settingsPopup_differentiatorsHaveDistinctPreviewColors()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(!src.isEmpty(), "qml/SettingsPopup.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("property var differentiatorPalette:")),
                 "must define a fixed-order categorical palette rather than generating hues ad hoc");
        QVERIFY2(src.contains(QStringLiteral("function differentiatorColor(")),
                 "must expose one lookup function so the palette has a single source of truth, "
                 "shared by the preview overlay and the button styling");

        // classifyFilenameHtml() must overlay per-group colors by matching
        // the REAL pattern (capturing parens already present), not just
        // literal/special classification — verify the overlay block exists
        // inside that function specifically, not somewhere unrelated.
        const qsizetype classifyIdx = src.indexOf(QStringLiteral("function classifyFilenameHtml("));
        QVERIFY2(classifyIdx >= 0, "classifyFilenameHtml() must still exist");
        const QString classifyBody = src.mid(classifyIdx, 4000);
        QVERIFY2(classifyBody.contains(QStringLiteral("differentiatorColor(diffGroup[idx])")),
                 "the filename-preview coloring must overlay differentiatorColor() per captured group, "
                 "not just the base literal/special/none classification");

        QVERIFY2(src.contains(QStringLiteral("settingsPopup.differentiatorColor(modelData.groupIndex)")),
                 "an ON GROUPING differentiator button must use the SAME palette color as its span in "
                 "the preview below, pairing color to a labeled button (the palette's CVD floor-band "
                 "warning's accessibility mitigation, not just decoration)");
        // A run marked as the SORT differentiator (see toggleDifferentiator()'s
        // OFF -> GROUPING -> SORT -> OFF cycle) deliberately does NOT get a
        // differentiatorColor() — that palette means "same color = same
        // group," which stops being true the moment a run is excluded from
        // the grouping key in favor of ordering by it instead.
        QVERIFY2(src.contains(QStringLiteral("isSortRun")),
                 "must track which run (if any) is the rule's sort differentiator so its button can be "
                 "styled/labeled distinctly from a grouping one");
    }

    // Regression guard for a real bug caught in review: two differentiator
    // runs can share byte-identical text (e.g. a device-id group and a
    // part-number group both written as plain "(\d+)" — an entirely
    // realistic pattern for this feature's own target use case). Matching
    // the sort differentiator by text ALONE would silently resolve to
    // whichever same-text run comes first, regardless of which one the user
    // actually picked. resolveSortGroupIndex()/toggleDifferentiator() must
    // also track/compare "occurrence" (the Nth same-text run, left to
    // right) to disambiguate. No QQuickView harness in this suite to drive
    // the actual JS logic, so this is a source-scan guard that the
    // disambiguation machinery still exists, not a behavioral test.
    void settingsPopup_sortDifferentiatorDisambiguatesDuplicateText()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(!src.isEmpty(), "qml/SettingsPopup.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("function resolveSortGroupIndex(pattern, sortGroupText, sortGroupOccurrence)")),
                 "resolveSortGroupIndex() must take an occurrence parameter alongside the run's text — "
                 "text alone can't distinguish two differentiator runs with identical text");
        QVERIFY2(src.contains(QStringLiteral("sortGroupOccurrence")),
                 "the occurrence must actually be threaded through and persisted on the rule (not just "
                 "accepted as a dead parameter) — commitPatternChange()/AppSettings.combineRules must "
                 "carry sortGroupOccurrence alongside sortGroupText");
    }

    // Item 38: the once-monolithic settings scroll is split into pages
    // selected from a left-hand navigation rail — currentPage gates which
    // section wrappers in contentColumn are visible, and the global chrome
    // (Reset to defaults / Change Vault) is lifted OUT of the paged content
    // into a persistent header so it stays reachable from every page. No
    // QQuickView in this suite to click the rail, so this is a source-scan
    // guard that the page state, the rail selector, the per-section page
    // gating, and the persistent chrome all exist rather than a behavioral
    // test.
    void settingsPopup_multiPageNavigation()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(!src.isEmpty(), "qml/SettingsPopup.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("property int currentPage")),
                 "the settings dialog must track which page is shown via a currentPage state (item 38)");
        QVERIFY2(src.contains(QStringLiteral("property var pageNames")),
                 "the page selector must enumerate its pages from a pageNames list, one rail entry each "
                 "(item 38)");
        QVERIFY2(src.contains(QStringLiteral("settingsPopup.currentPage = navButton.index")),
                 "each navigation-rail entry must switch the shown page by setting currentPage (item 38)");

        // The content must be partitioned into multiple page-gated section
        // wrappers, not one giant scroll — count the currentPage gates.
        int idx = 0;
        int pageGates = 0;
        while ((idx = src.indexOf(QStringLiteral("visible: settingsPopup.currentPage === "), idx)) >= 0) {
            ++pageGates;
            idx += 1;
        }
        QVERIFY2(pageGates >= 4,
                 qPrintable(QStringLiteral("the settings content must be partitioned into multiple "
                                           "page-gated section wrappers (found %1 currentPage gates, "
                                           "expected >= 4) (item 38)").arg(pageGates)));

        // Persistent chrome: Reset to defaults + Change Vault must live in a
        // header row OUTSIDE the paged/scrolling content so they're reachable
        // from every page.
        const int headerIdx = src.indexOf(QStringLiteral("id: headerRow"));
        QVERIFY2(headerIdx >= 0, "a persistent header chrome row (headerRow) must exist (item 38)");
        const int scrollIdx = src.indexOf(QStringLiteral("id: scrollArea"));
        QVERIFY2(scrollIdx > headerIdx,
                 "the persistent header chrome must precede the scrolling page content (item 38)");
        const QString headerBlock = src.mid(headerIdx, scrollIdx - headerIdx);
        QVERIFY2(headerBlock.contains(QStringLiteral("id: resetAllButton"))
                     && headerBlock.contains(QStringLiteral("id: changeVaultButton")),
                 "Reset to defaults and Change Vault must live in the persistent header chrome "
                 "(outside the paged content) so they stay reachable from every page (item 38)");
    }

    // Item 40 (folder management): Sidebar.qml must expose a right-click
    // context menu that creates + deletes folders and moves a file into
    // another folder, wired to the new VaultTreeModel invokables
    // (createFolder/deleteFolder/moveFile/vaultFolders). No QQuickView in
    // this suite to actually right-click a TreeView row, so this is a
    // source-scan guard that the menu, the right-click triggers, and the
    // invokable wiring all exist rather than a behavioral test.
    void sidebar_folderManagementContextMenu()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        // Right-click triggers: a per-row menu (treeRowContextTap) and an
        // empty-area one (treeEmptyContextTap for the vault root), both on
        // the right mouse button.
        QVERIFY2(src.contains(QStringLiteral("id: treeRowContextTap")),
                 "each tree row must have a right-click TapHandler to open the folder-management menu");
        QVERIFY2(src.contains(QStringLiteral("id: treeEmptyContextTap")),
                 "empty tree space must have a right-click TapHandler so \"New folder\" at the vault "
                 "root is reachable with no row to click");
        const int rowTapIdx = src.indexOf(QStringLiteral("id: treeRowContextTap"));
        QVERIFY2(src.mid(rowTapIdx, 200).contains(QStringLiteral("acceptedButtons: Qt.RightButton")),
                 "the row context TapHandler must accept the right button");

        // The three actions must exist as menu rows.
        QVERIFY2(src.contains(QStringLiteral("id: contextMenu")),
                 "a context-menu Popup (contextMenu) must exist");
        QVERIFY2(src.contains(QStringLiteral("id: ctxNewFolderRow"))
                     && src.contains(QStringLiteral("id: ctxMoveRow"))
                     && src.contains(QStringLiteral("id: ctxDeleteRow")),
                 "the context menu must offer New folder / Move to… / Delete folder rows");
        // Delete only for a directory row; Move only for a note row.
        const int moveIdx = src.indexOf(QStringLiteral("id: ctxMoveRow"));
        QVERIFY2(src.mid(moveIdx, 120).contains(QStringLiteral("visible: sidebar.ctxIsNote")),
                 "\"Move to…\" must only be offered for a note (audio/orphaned .md) row");
        const int delIdx = src.indexOf(QStringLiteral("id: ctxDeleteRow"));
        QVERIFY2(src.mid(delIdx, 120).contains(QStringLiteral("visible: sidebar.ctxIsDir")),
                 "\"Delete folder\" must only be offered for a directory row");

        // The actions must call the new VaultTreeModel invokables (with the
        // file:// scheme — a raw local path makes QUrl::toLocalFile() empty).
        QVERIFY2(src.contains(QStringLiteral("sidebar.model.createFolder(")),
                 "New folder must call VaultTreeModel.createFolder()");
        QVERIFY2(src.contains(QStringLiteral("sidebar.model.deleteFolder(")),
                 "Delete folder must call VaultTreeModel.deleteFolder()");
        QVERIFY2(src.contains(QStringLiteral("sidebar.model.moveFile(")),
                 "Move to… must call VaultTreeModel.moveFile()");
        QVERIFY2(src.contains(QStringLiteral("sidebar.model.vaultFolders()")),
                 "the Move to… picker must enumerate destinations via VaultTreeModel.vaultFolders()");
        QVERIFY2(src.contains(QStringLiteral("\"file://\" + sidebar.ctxCreateInDir"))
                     || src.contains(QStringLiteral("\"file://\" + sidebar.ctxPath")),
                 "the invokable calls must pass file:// URLs, not raw local paths");

        // Deleting a folder is destructive — must go through a confirmation.
        QVERIFY2(src.contains(QStringLiteral("id: deleteConfirmPopup")),
                 "deleting a folder (recursive) must require a confirmation Popup");
    }

    // CLAUDE.md load-bearing constraint: Qt Multimedia only *plays* audio and
    // must use the ffmpeg backend — forced before QGuiApplication exists.
    void mainCpp_forcesFfmpegMediaBackend()
    {
        const QString src = readAll(QStringLiteral(SUOP_SRC_DIR "/main.cpp"));
        const qsizetype setEnv = src.indexOf(QStringLiteral("qputenv(\"QT_MEDIA_BACKEND\", \"ffmpeg\")"));
        const qsizetype appCtor = src.indexOf(QStringLiteral("QGuiApplication app("));
        QVERIFY2(setEnv >= 0, "main.cpp must force QT_MEDIA_BACKEND=ffmpeg (CLAUDE.md)");
        QVERIFY2(appCtor > setEnv, "backend must be forced before QGuiApplication is constructed");
    }

    // Milestone 2 decision: the desktop's QQC2 theme (KDE Breeze) breaks this
    // app; the bundled Basic style is forced instead.
    void mainCpp_forcesBasicControlsStyle()
    {
        const QString src = readAll(QStringLiteral(SUOP_SRC_DIR "/main.cpp"));
        const qsizetype setEnv = src.indexOf(QStringLiteral("qputenv(\"QT_QUICK_CONTROLS_STYLE\", \"Basic\")"));
        const qsizetype appCtor = src.indexOf(QStringLiteral("QGuiApplication app("));
        QVERIFY(setEnv >= 0);
        QVERIFY(appCtor > setEnv);
    }

    // qt-window-effects constraint: transparency needs an 8-bit alpha surface
    // format installed as the default before QGuiApplication exists.
    void mainCpp_alphaSurfaceBeforeApp()
    {
        const QString src = readAll(QStringLiteral(SUOP_SRC_DIR "/main.cpp"));
        const qsizetype alpha = src.indexOf(QStringLiteral("setAlphaBufferSize(8)"));
        const qsizetype appCtor = src.indexOf(QStringLiteral("QGuiApplication app("));
        QVERIFY(alpha >= 0);
        QVERIFY(appCtor > alpha);
    }

    // StyleGuide.md: every interactable gets Qt.PointingHandCursor + a hover
    // outline. "Every interactable" is not statically enumerable, so this
    // documents the rule and sanity-checks that the convention is in use at
    // all rather than proving completeness.
    void hoverCursor_conventionInUse()
    {
        const QDir qmlDir(QStringLiteral(SUOP_QML_DIR));
        int cursorFiles = 0;
        int hoverBorderUses = 0;
        const QStringList files = qmlDir.entryList({QStringLiteral("*.qml")}, QDir::Files);
        for (const QString &name : files) {
            if (name == QLatin1String("HoverBorder.qml"))
                continue;
            const QString src = readAll(qmlDir.filePath(name));
            if (src.contains(QStringLiteral("PointingHandCursor")))
                ++cursorFiles;
            if (src.contains(QStringLiteral("HoverBorder {")))
                ++hoverBorderUses;
        }
        QVERIFY2(cursorFiles >= 4, "pointer-cursor convention should appear across the UI files");
        QVERIFY2(hoverBorderUses >= 3, "HoverBorder should be the reused hover-outline component");
    }

    // Feature: Up/Down should move to the previous/next note (audio file or
    // orphaned .md) when a sidebar row has keyboard focus, in both the
    // normal TreeView and the flat searchResultsView shown during a text
    // search. No QQuickView in this test suite to actually deliver key
    // events to a TreeView delegate, so this is a source-scan guard: the
    // Shortcut items must exist, gated on the actual view having focus
    // (not sidebar.currentPath, which is MainPanel's "currently open" state
    // and must stay untouched by this), and both TapHandlers must grab
    // focus on click so a freshly-clicked row is what Up/Down moves from.
    void sidebar_arrowKeyNavigatesNotes()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("function moveNoteSelection(direction)")),
                 "Sidebar.qml must define moveNoteSelection() to back Up/Down navigation");

        const int upIdx = src.indexOf(QStringLiteral("sequence: \"Up\""));
        const int downIdx = src.indexOf(QStringLiteral("sequence: \"Down\""));
        QVERIFY2(upIdx >= 0, "Up must be wired as a Shortcut");
        QVERIFY2(downIdx >= 0, "Down must be wired as a Shortcut");
        const QString upBlock = src.mid(upIdx, 200);
        const QString downBlock = src.mid(downIdx, 200);
        QVERIFY2(upBlock.contains(QStringLiteral("tree.activeFocus || searchResultsView.activeFocus")),
                 "Up shortcut must be gated on the tree/search-results view actually having focus, "
                 "not on sidebar.currentPath (MainPanel's open-file state, unrelated to keyboard focus)");
        QVERIFY2(downBlock.contains(QStringLiteral("tree.activeFocus || searchResultsView.activeFocus")),
                 "Down shortcut must be gated the same way as Up");
        QVERIFY2(upBlock.contains(QStringLiteral("moveNoteSelection(-1)")), "Up must move backward");
        QVERIFY2(downBlock.contains(QStringLiteral("moveNoteSelection(1)")), "Down must move forward");

        QVERIFY2(src.contains(QStringLiteral("treeDelegate.treeView.forceActiveFocus()")),
                 "clicking a tree row must grab keyboard focus so Up/Down has something to move from");
        QVERIFY2(src.contains(QStringLiteral("searchResultsView.forceActiveFocus()")),
                 "clicking a search-result row must grab keyboard focus so Up/Down works there too");

        // Moving to an offscreen note must scroll it into view, in both
        // the TreeView and the flat search-results ListView.
        QVERIFY2(src.contains(QStringLiteral("tree.positionViewAtRow(notes[idx].row, TableView.Contain)")),
                 "moveNoteSelection must scroll TreeView to reveal the newly-selected row");
        QVERIFY2(src.contains(QStringLiteral("searchResultsView.positionViewAtIndex(idx, ListView.Contain)")),
                 "moveNoteSelection must scroll searchResultsView to reveal the newly-selected row");
    }

    // Both auto-label sliders (tag reuse bias, label wordiness) only ever
    // drive 3-tier qualitative phrasing in LabelingEngine.cpp's
    // buildSystemPrompt() (low/mid/high, thresholds at 0.34/0.67) — a
    // continuous 0.0-1.0 drag implies finer control than the prompt actually
    // has. They must snap to exactly those 3 positions (0, 0.5, 1) rather
    // than free-dragging. No QQuickView in this suite to drive the actual
    // Slider, so this is a source-scan guard that stepSize/snapMode are set.
    void settingsPopup_autoLabelSlidersSnapToDiscreteTiers()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(!src.isEmpty(), "qml/SettingsPopup.qml must exist");

        for (const QString &sliderId : {QStringLiteral("tagReuseBiasSlider"), QStringLiteral("wordinessSlider")}) {
            const int idx = src.indexOf(QStringLiteral("id: %1").arg(sliderId));
            QVERIFY2(idx >= 0, qPrintable(sliderId + QStringLiteral(" must exist")));
            const QString block = src.mid(idx, 400);
            QVERIFY2(block.contains(QStringLiteral("stepSize: 0.5")),
                     qPrintable(sliderId
                                + QStringLiteral(" must use stepSize: 0.5 to match the 3-tier prompt phrasing "
                                                  "(low/mid/high) — no finer granularity actually exists")));
            QVERIFY2(block.contains(QStringLiteral("snapMode: Slider.SnapAlways")),
                     qPrintable(sliderId
                                + QStringLiteral(" must use snapMode: Slider.SnapAlways so dragging actually "
                                                  "lands on a tier instead of an intermediate value")));
        }
    }

    // Each auto-label slider must show a live subtitle naming the actual
    // system-prompt clause its current position selects — otherwise the
    // 3-tier snap above just looks like an arbitrary low/mid/high knob with
    // no visible effect. wordinessPhrasing()/tagReuseBiasPhrasing() are a
    // manually-duplicated mirror of buildSystemPrompt()/labelPreludePhrasing()
    // in LabelingEngine.cpp (QML can't call into that C++ string table), so
    // this also guards that the preview functions still exist and are wired
    // to the right slider's live value, not a stale/disconnected one.
    void settingsPopup_autoLabelSlidersShowPromptPreview()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(!src.isEmpty(), "qml/SettingsPopup.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("function wordinessPhrasing(value)")),
                 "wordinessPhrasing() must exist to preview the label/prelude verbosity clause");
        QVERIFY2(src.contains(QStringLiteral("function tagReuseBiasPhrasing(value)")),
                 "tagReuseBiasPhrasing() must exist to preview the tag-reuse clause");

        QVERIFY2(src.contains(QStringLiteral("settingsPopup.wordinessPhrasing(wordinessSlider.value)")),
                 "the wordiness subtitle must read wordinessSlider's own live value, not AppSettings.wordiness "
                 "(which would lag a drag until onMoved commits it)");
        QVERIFY2(src.contains(QStringLiteral("settingsPopup.tagReuseBiasPhrasing(tagReuseBiasSlider.value)")),
                 "the tag-reuse subtitle must read tagReuseBiasSlider's own live value");
    }

    // Item 3: Shift/Ctrl-click multi-select, layered on top of the existing
    // single "currently open" pointer (currentPath/O7). No QQuickView in
    // this suite to actually deliver a modified click to a TreeView
    // delegate, so this is a source-scan guard that the selection state,
    // the Shift-range/Ctrl-toggle logic, both TapHandlers' wiring, and the
    // three bulk-ribbon buttons' "selected-set-when-2+-else-everything"
    // narrowing all still exist.
    void sidebar_multiSelectShiftCtrlClick()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("property var selectedPaths: []")),
                 "Sidebar.qml must hold a flat selectedPaths array, separate from the single-file "
                 "currentPath pointer");
        QVERIFY2(src.contains(QStringLiteral("function handleNoteClick(path, isOrphanedMd, modifiers)")),
                 "must define one shared click-handling entry point for both the tree and search-results "
                 "delegates");

        const int handlerIdx = src.indexOf(QStringLiteral("function handleNoteClick("));
        QVERIFY(handlerIdx >= 0);
        const QString handlerBody = src.mid(handlerIdx, 1600);
        QVERIFY2(handlerBody.contains(QStringLiteral("Qt.ShiftModifier")),
                 "handleNoteClick must check Qt.ShiftModifier for range-select");
        QVERIFY2(handlerBody.contains(QStringLiteral("Qt.ControlModifier")),
                 "handleNoteClick must check Qt.ControlModifier for toggle-select");
        QVERIFY2(handlerBody.contains(QStringLiteral("sidebar.visibleNotePaths()")),
                 "Shift-range must walk the currently visible/flattened note order, not a raw tree walk "
                 "that could include a collapsed directory's hidden children");
        QVERIFY2(handlerBody.contains(QStringLiteral("order.slice(lo, hi + 1)")),
                 "Shift-click must select the contiguous range between the last-clicked note and the "
                 "one just clicked");
        QVERIFY2(handlerBody.contains(QStringLiteral(".filter((p) => p !== path)"))
                     && handlerBody.contains(QStringLiteral(".concat([path])")),
                 "Ctrl-click must toggle exactly the clicked path in/out of selectedPaths without "
                 "clearing the rest of the selection");

        // Both TapHandlers (treeDelegate's and resultDelegate's) must route
        // through handleNoteClick with their own point.modifiers.
        QVERIFY2(src.contains(QStringLiteral("treeRowTap.point.modifiers")),
                 "treeDelegate's TapHandler must pass its own point.modifiers into handleNoteClick");
        QVERIFY2(src.contains(QStringLiteral("resultRowTap.point.modifiers")),
                 "searchResultsView's resultDelegate TapHandler must pass its own point.modifiers into "
                 "handleNoteClick");

        // Bulk ribbon buttons: with 2+ selected, narrow to the selection
        // (intersected with eligibility); with 0/1 selected, keep acting on
        // everything eligible (existing pre-item-3 behavior).
        QVERIFY2(src.contains(QStringLiteral("function bulkTargetPaths(eligibleMap)")),
                 "must define one shared helper for the selected-vs-everything-eligible narrowing");
        QVERIFY2(src.contains(QStringLiteral("sidebar.bulkTargetPaths(sidebar.untranscribedPaths)")),
                 "transcribeAllButton must narrow to the selection via bulkTargetPaths");
        QVERIFY2(src.contains(QStringLiteral("sidebar.bulkTargetPaths(sidebar.transcribedPaths)")),
                 "relabelAllButton must narrow to the selection via bulkTargetPaths");
        QVERIFY2(src.contains(QStringLiteral("sidebar.bulkTargetPaths(sidebar.chaptersPaths)")),
                 "chaptersAllButton must narrow to the selection via bulkTargetPaths");

        // "N selected" indicator, only for a genuine multi-selection (2+).
        QVERIFY2(src.contains(QStringLiteral("id: multiSelectCountLabel")),
                 "must show an \"N selected\" label near the ribbon");
        const int labelIdx = src.indexOf(QStringLiteral("id: multiSelectCountLabel"));
        QVERIFY(labelIdx >= 0);
        const QString labelBlock = src.mid(labelIdx, 300);
        QVERIFY2(labelBlock.contains(QStringLiteral("sidebar.selectedPaths.length > 1")),
                 "the \"N selected\" label must only show for a genuine multi-selection (2+), not a "
                 "single selected row (already covered by the existing O7 currently-open outline)");
    }

    // Item 44: manual "combine selected" — the sidebar must offer a way to
    // combine the current multi-selection, gated on 2+ AUDIO files being
    // selected, and route it through the controller's combineSelected(). No
    // QQuickView in this suite to open the context menu or click the button,
    // so this is a source-scan guard that the helper, the audio-only
    // narrowing, the >= 2 gate, and the controller call all still exist.
    void sidebar_combineSelectedActionGatedOnTwoAudio()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        // The selection is narrowed to audio-only before combining (an
        // orphaned .md can be multi-selected but can't be concatenated).
        QVERIFY2(src.contains(QStringLiteral("function selectedAudioPaths()")),
                 "Sidebar.qml must define selectedAudioPaths() to narrow the multi-selection to audio "
                 "notes for combine");
        QVERIFY2(src.contains(QStringLiteral("function combineSelectedFiles()")),
                 "Sidebar.qml must define combineSelectedFiles() as the single entry point that hands the "
                 "selection to the controller");

        // The combine call itself must route through controller.combineSelected.
        const int fnIdx = src.indexOf(QStringLiteral("function combineSelectedFiles()"));
        QVERIFY(fnIdx >= 0);
        const QString fnBody = src.mid(fnIdx, 700);
        QVERIFY2(fnBody.contains(QStringLiteral(".combineSelected(")),
                 "combineSelectedFiles() must call the controller's combineSelected()");
        QVERIFY2(fnBody.contains(QStringLiteral("sidebar.selectedAudioPaths()"))
                     && fnBody.contains(QStringLiteral("< 2")),
                 "combineSelectedFiles() must gate on 2+ audio files (selectedAudioPaths().length < 2 -> "
                 "no-op) before calling the controller");

        // The context-menu entry — the must-have home — must exist and be
        // gated on 2+ audio files selected.
        const int ctxIdx = src.indexOf(QStringLiteral("id: ctxCombineSelectedRow"));
        QVERIFY2(ctxIdx >= 0, "context menu must have a \"Combine selected\" row (ctxCombineSelectedRow)");
        const QString ctxBlock = src.mid(ctxIdx, 900);
        QVERIFY2(ctxBlock.contains(QStringLiteral("sidebar.selectedAudioPaths().length >= 2")),
                 "the \"Combine selected\" context row must be visible only when 2+ audio files are "
                 "selected");
        QVERIFY2(ctxBlock.contains(QStringLiteral("sidebar.combineSelectedFiles()")),
                 "the \"Combine selected\" context row must call combineSelectedFiles() on tap");
    }

    // Item 49: closing search must also clear the tag filter — otherwise
    // rows stay hidden by a filter the user can no longer see or edit. No
    // QQuickView in this suite to actually click searchCloseButton, so this
    // is a source-scan guard.
    void sidebar_searchCloseClearsTagFilter()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        const int idx = src.indexOf(QStringLiteral("id: searchCloseButton"));
        QVERIFY2(idx >= 0, "searchCloseButton must exist");
        const QString block = src.mid(idx, 1000);
        QVERIFY2(block.contains(QStringLiteral("sidebar.searchQuery = \"\"")),
                 "searchCloseButton must still clear the search query");
        QVERIFY2(block.contains(QStringLiteral("tagFilterField.text = \"\""))
                     && block.contains(QStringLiteral("sidebar.tagFilterText = \"\"")),
                 "searchCloseButton must also clear the tag filter (tagFilterField.text and "
                 "sidebar.tagFilterText) so closing search fully clears filtering, not just the text "
                 "query");
    }

    // Item 22: the tag-filter chip itself (not just its embedded remove
    // icon, which already had this) must show the pointer cursor on hover —
    // StyleGuide.md's "every interactable" hover-cursor convention.
    void sidebar_tagChipHasPointerCursor()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        const int idx = src.indexOf(QStringLiteral("id: tagChipHover"));
        QVERIFY2(idx >= 0, "tagChipHover must exist");
        const QString block = src.mid(idx, 120);
        QVERIFY2(block.contains(QStringLiteral("cursorShape: Qt.PointingHandCursor")),
                 "tagChipHover must set cursorShape: Qt.PointingHandCursor (StyleGuide.md hover convention)");
    }

    // Item 5: an untranscribed note's speaker-detection toggle must reflect
    // the user's saved vault-wide default (AppSettings.defaultSpeakerDetection),
    // exactly as the model picker effectively does — NOT a hardcoded `true`,
    // which discarded the saved default on every fresh-note switch. No
    // QQuickView in this suite to drive refreshMetadata() across a file
    // switch, so this is a source-scan guard that the fallback reads the
    // setting rather than a literal.
    void mainPanel_speakerDetectionFallbackHonorsSavedDefault()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const int idx = src.indexOf(QStringLiteral("speakerDetectionToggle.checked ="));
        QVERIFY2(idx >= 0, "refreshMetadata() must set speakerDetectionToggle.checked");
        const QString block = src.mid(idx, 200);
        QVERIFY2(block.contains(QStringLiteral("storedMetadata.speakerDetection === true")),
                 "an already-transcribed note must still honor its stored speakerDetection");
        QVERIFY2(block.contains(QStringLiteral("AppSettings.defaultSpeakerDetection")),
                 "the !hasStoredMetadata fallback must reflect AppSettings.defaultSpeakerDetection, "
                 "not a hardcoded true — otherwise a fresh-note switch discards the user's saved default "
                 "(item 5)");
    }

    // Item 4: the model picker's CLOSED-state box must round its corners with
    // AppSettings.borderRadius — QQC2's default ComboBox background is
    // square-cornered, so modelCombo needs its own background: Rectangle
    // override (the dropdown popup was already fixed separately). No
    // QQuickView here to inspect the rendered corners, so this is a
    // source-scan guard that the background override exists.
    void mainPanel_modelComboClosedBoxIsRounded()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const int idx = src.indexOf(QStringLiteral("id: modelCombo"));
        QVERIFY2(idx >= 0, "modelCombo must exist");
        // Scan only up to the popup override so this asserts the closed-box
        // background specifically, not the already-fixed popup background.
        const int popupIdx = src.indexOf(QStringLiteral("popup: Popup"), idx);
        QVERIFY2(popupIdx > idx, "modelCombo's popup override must exist after it");
        const QString closedBoxBlock = src.mid(idx, popupIdx - idx);
        QVERIFY2(closedBoxBlock.contains(QStringLiteral("background: Rectangle")),
                 "modelCombo must override its closed-state background with a Rectangle (QQC2's default "
                 "box is square-cornered) — item 4");
        const int bgIdx = closedBoxBlock.indexOf(QStringLiteral("background: Rectangle"));
        const QString bgBlock = closedBoxBlock.mid(bgIdx, 200);
        QVERIFY2(bgBlock.contains(QStringLiteral("radius: AppSettings.borderRadius")),
                 "modelCombo's closed-state background must round its corners with "
                 "AppSettings.borderRadius (StyleGuide.md border-radius convention)");
    }

    // Item 14: the progress glow is now on by default, and its halo layer
    // count is user-configurable. No QQuickView here to render the glow, so
    // these are source-scan guards: (a) progressGlowEnabled defaults true in
    // both the property and the defaults map; (b) BottomBar.qml builds the
    // halos with a Repeater driven by AppSettings.progressGlowLayerCount
    // rather than the old two hand-written halo Rectangles.
    void progressGlow_enabledByDefaultAndLayerCountDriven()
    {
        const QString settings = readAll(QStringLiteral(SUOP_QML_DIR "/AppSettings.qml"));
        QVERIFY2(!settings.isEmpty(), "qml/AppSettings.qml must exist");
        QVERIFY2(settings.contains(QStringLiteral("property bool progressGlowEnabled: true")),
                 "progressGlowEnabled must default true (item 14)");
        QVERIFY2(settings.contains(QStringLiteral("progressGlowEnabled: true")),
                 "the defaults map must also default progressGlowEnabled true (Settings reset "
                 "must restore the glow, item 14)");
        QVERIFY2(settings.contains(QStringLiteral("property int progressGlowLayerCount: 2")),
                 "progressGlowLayerCount must exist and default 2 (matches the original two-halo "
                 "appearance, item 14)");
        QVERIFY2(settings.contains(QStringLiteral("progressGlowLayerCount: 2")),
                 "the defaults map must include progressGlowLayerCount so reset/persist work");
        QVERIFY2(settings.contains(QStringLiteral("function onProgressGlowLayerCountChanged()")),
                 "progressGlowLayerCount needs its own save hook (persistence correctness)");

        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");
        const int glowIdx = bar.indexOf(QStringLiteral("id: progressGlow"));
        QVERIFY2(glowIdx >= 0, "progressGlow Item must exist");
        const QString glowBlock = bar.mid(glowIdx, 2400);
        QVERIFY2(glowBlock.contains(QStringLiteral("Repeater")),
                 "the halo layers must be generated by a Repeater (item 14), not hand-written");
        QVERIFY2(glowBlock.contains(QStringLiteral("model: AppSettings.progressGlowLayerCount")),
                 "the halo Repeater must be driven by AppSettings.progressGlowLayerCount (item 14)");
    }

    // Items 15/54: when the glow is on and progressGlowHidesBar is on (both
    // default true), the standard ProgressBar hides so the glow reports alone;
    // progressGlowHidesBar is an independent, default-on toggle so a user can
    // show both. Source-scan guards: the setting exists/defaults true with its
    // save hook, and the ProgressBar's visible binding references it.
    void progressGlow_hidesStandardBarWhenEnabled()
    {
        const QString settings = readAll(QStringLiteral(SUOP_QML_DIR "/AppSettings.qml"));
        QVERIFY2(!settings.isEmpty(), "qml/AppSettings.qml must exist");
        QVERIFY2(settings.contains(QStringLiteral("property bool progressGlowHidesBar: true")),
                 "progressGlowHidesBar must exist and default true (item 54)");
        QVERIFY2(settings.contains(QStringLiteral("progressGlowHidesBar: true")),
                 "the defaults map must include progressGlowHidesBar so reset/persist work");
        QVERIFY2(settings.contains(QStringLiteral("function onProgressGlowHidesBarChanged()")),
                 "progressGlowHidesBar needs its own save hook (persistence correctness)");

        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");
        const int pbIdx = bar.indexOf(QStringLiteral("ProgressBar {"));
        QVERIFY2(pbIdx >= 0, "the standard ProgressBar must exist");
        const QString pbBlock = bar.mid(pbIdx, 1600);
        QVERIFY2(pbBlock.contains(QStringLiteral("AppSettings.progressGlowEnabled"))
                     && pbBlock.contains(QStringLiteral("AppSettings.progressGlowHidesBar")),
                 "the ProgressBar's visible binding must yield to the glow via "
                 "progressGlowEnabled && progressGlowHidesBar (items 15/54)");
    }

    // Wave (task-queue overhaul) source-scan guards for BottomBar.qml — no
    // QQuickView harness here, so these assert the checked-in QML the same
    // way the progress-glow guards above do.
    //
    // Item 9: the bottom bar shows ONLY a pause/resume button. The redundant
    // stop/cancel button (id: stopButton, ⏹) was removed — cancel lives in
    // the Tasks popup. And the pause button is greyed (enabled bound to a
    // canPauseNow() gate), never silently reinterpreting a "loading"-phase
    // tap as a cancel.
    void bottomBar_singlePauseButton_noStopButton_pauseGreyedNotCancel()
    {
        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");
        QVERIFY2(!bar.contains(QStringLiteral("id: stopButton")),
                 "the redundant bottom-bar stopButton must be removed (item 9) — cancel "
                 "lives in the Tasks popup");
        QVERIFY2(!bar.contains(QStringLiteral("onTapped: bottomBar.transcriptionController.cancelActive()")),
                 "the bottom bar must no longer wire a tap to cancelActive() (item 9) — the "
                 "stop button that did is gone");
        QVERIFY2(bar.contains(QStringLiteral("function canPauseNow()")),
                 "a shared canPauseNow() gate must exist (item 9)");
        QVERIFY2(bar.contains(QStringLiteral("enabled: bottomBar.canPauseNow()")),
                 "the pause button must be enabled/greyed via canPauseNow() (item 9) rather "
                 "than always-enabled");
        QVERIFY2(bar.contains(QStringLiteral("bottomBar.currentPhase !== \"loading\"")),
                 "canPauseNow() must grey the pause button during the uninterruptible "
                 "\"loading\" phase (item 9)");
    }

    // Items 10/11: the Tasks popup's ListView binds to the pre-partitioned
    // sortedJobs view (active/queued first, finished demoted) — NOT the old
    // jobs().reverse() that inverted run order and never separated finished
    // work.
    void bottomBar_taskListBindsSortedJobs_notReversedRawJobs()
    {
        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");
        const int lvIdx = bar.indexOf(QStringLiteral("id: jobListView"));
        QVERIFY2(lvIdx >= 0, "jobListView must exist");
        const QString lvBlock = bar.mid(lvIdx, 1600);
        QVERIFY2(lvBlock.contains(QStringLiteral("transcriptionController.sortedJobs")),
                 "the Tasks ListView must bind its model to sortedJobs (items 10/11)");
        QVERIFY2(!lvBlock.contains(QStringLiteral(".reverse()")),
                 "the Tasks ListView must not blindly reverse raw jobs() any more "
                 "(items 10/11) — ordering is done in sortedJobs");
    }

    // Item 36: the task counter is always visible (reads "0/0 tasks" when
    // idle) — its Item must NOT be gated on tasksTotal > 0.
    void bottomBar_tasksCounterAlwaysVisible()
    {
        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");
        const int idx = bar.indexOf(QStringLiteral("id: tasksButton"));
        QVERIFY2(idx >= 0, "tasksButton must exist");
        const QString block = bar.mid(idx, 400);
        QVERIFY2(!block.contains(QStringLiteral("visible: bottomBar.tasksTotal > 0")),
                 "tasksButton must no longer be gated on tasksTotal > 0 (item 36) — the "
                 "counter stays visible, reading 0/0 when idle");
    }

    // Item 25: the bottom bar's backend indicator shows the coarse
    // "gpu"/"cpu" kind (activeBackendKind), not the raw Vulkan/CUDA/CPU name.
    void bottomBar_backendIndicatorUsesKind()
    {
        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");
        QVERIFY2(bar.contains(QStringLiteral("activeBackendKind")),
                 "the bottom bar backend indicator must use activeBackendKind (item 25)");
    }

    // Item 37: the async drag-and-drop "import" job kind flows through the
    // same job-list rendering as every other kind, so its kind badge
    // (kindLabel) and phase label (phaseLabel) must both handle it — no
    // QQuickView here to render the delegate, so scan the two switch helpers'
    // source instead (same technique the other BottomBar guards use).
    void bottomBar_importKindHasBadgeAndPhaseLabel()
    {
        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");

        const int kindIdx = bar.indexOf(QStringLiteral("function kindLabel"));
        QVERIFY2(kindIdx >= 0, "kindLabel() must exist");
        const QString kindBlock = bar.mid(kindIdx, 600);
        QVERIFY2(kindBlock.contains(QStringLiteral("case \"import\":")),
                 "kindLabel() must have an \"import\" case (item 37) so import jobs get a "
                 "readable kind badge in the Tasks popup");

        const int phaseIdx = bar.indexOf(QStringLiteral("function phaseLabel"));
        QVERIFY2(phaseIdx >= 0, "phaseLabel() must exist");
        const QString phaseBlock = bar.mid(phaseIdx, 900);
        QVERIFY2(phaseBlock.contains(QStringLiteral("case \"importing\":")),
                 "phaseLabel() must have an \"importing\" case (item 37) so an active import "
                 "shows a phase label instead of a blank");
    }

    // Item 52 (diarize kind) + item 26/31 (chaptering phase): the two recently
    // added job kinds/phases must have label cases so their Tasks badge / phase
    // text isn't blank. The standalone diarize job is kind "diarize"; chapter
    // generation runs a "chaptering" phase.
    void bottomBar_diarizeKindAndChapteringPhaseLabels()
    {
        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");

        const int kindIdx = bar.indexOf(QStringLiteral("function kindLabel"));
        QVERIFY2(kindIdx >= 0, "kindLabel() must exist");
        QVERIFY2(bar.mid(kindIdx, 500).contains(QStringLiteral("case \"diarize\":")),
                 "kindLabel() must have a \"diarize\" case (item 52) so standalone "
                 "detect-speakers jobs get a readable kind badge in the Tasks popup");

        const int phaseIdx = bar.indexOf(QStringLiteral("function phaseLabel"));
        QVERIFY2(phaseIdx >= 0, "phaseLabel() must exist");
        QVERIFY2(bar.mid(phaseIdx, 1000).contains(QStringLiteral("case \"chaptering\":")),
                 "phaseLabel() must have a \"chaptering\" case (item 26/31) so an active "
                 "chapter-generation phase shows a phase label instead of a blank");
    }

    // Item 29 + item 21: the popup is widened so descriptions fit, and its
    // vertical padding is symmetric (one innerPadding used for both the
    // ColumnLayout margin and the height calc, no artificial min-height floor
    // leaving uneven slack below the list).
    void bottomBar_popupWiderAndSymmetricPadding()
    {
        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");
        const int idx = bar.indexOf(QStringLiteral("id: jobListPopup"));
        QVERIFY2(idx >= 0, "jobListPopup must exist");
        const QString block = bar.mid(idx, 1600);
        const QRegularExpression widthRe(QStringLiteral("\\bwidth:\\s*(\\d+)"));
        const QRegularExpressionMatch wm = widthRe.match(block);
        QVERIFY2(wm.hasMatch(), "jobListPopup must set an explicit width");
        QVERIFY2(wm.captured(1).toInt() >= 460,
                 "the Tasks popup must be widened to >=460 so descriptions aren't "
                 "truncated (item 29)");
        QVERIFY2(block.contains(QStringLiteral("innerPadding")),
                 "the popup must use a named symmetric innerPadding for its inner spacing "
                 "(item 21)");
        QVERIFY2(!block.contains(QStringLiteral("Math.max(jobColumn.implicitHeight")),
                 "the popup height must no longer floor on a min height (item 21) — that "
                 "floor was the source of uneven slack below the list");
    }

    // Item 42: clicking the bottom-left current-file path label must focus the
    // IN-APP file explorer (the Sidebar tree) and reveal the open file there,
    // NOT shell out to the OS file manager (revealInFileManager). No QQuickView
    // here to click the label and observe focus, so this is a source-scan guard
    // covering the full BottomBar -> Main -> Sidebar wiring: BottomBar declares
    // the upward signal and its label tap emits it (and no longer calls
    // revealInFileManager); Main wires that signal to a Sidebar focus-and-reveal
    // function; Sidebar defines that function and it takes tree focus.
    void bottomBar_fileLabelFocusesInAppExplorer()
    {
        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");
        QVERIFY2(bar.contains(QStringLiteral("signal focusFileExplorerRequested")),
                 "BottomBar.qml must declare focusFileExplorerRequested(...) so the "
                 "current-file label routes its click up to Main (item 42)");
        QVERIFY2(bar.contains(QStringLiteral("focusFileExplorerRequested(bottomBar.currentFilePath)")),
                 "the current-file label's TapHandler must emit focusFileExplorerRequested "
                 "(item 42)");
        QVERIFY2(!bar.contains(QStringLiteral("revealInFileManager")),
                 "the current-file label must no longer shell out to the OS file manager "
                 "via revealInFileManager (item 42)");

        const QString main = readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml"));
        QVERIFY2(!main.isEmpty(), "qml/Main.qml must exist");
        QVERIFY2(main.contains(QStringLiteral("onFocusFileExplorerRequested"))
                     && main.contains(QStringLiteral("sidebar.focusAndRevealNote")),
                 "Main.qml must wire BottomBar's focusFileExplorerRequested to "
                 "sidebar.focusAndRevealNote(...) (item 42)");

        const QString side = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!side.isEmpty(), "qml/Sidebar.qml must exist");
        const int fnIdx = side.indexOf(QStringLiteral("function focusAndRevealNote"));
        QVERIFY2(fnIdx >= 0, "Sidebar.qml must define focusAndRevealNote() (item 42)");
        const QString fnBlock = side.mid(fnIdx, 800);
        QVERIFY2(fnBlock.contains(QStringLiteral("forceActiveFocus")),
                 "focusAndRevealNote() must give the file explorer keyboard focus "
                 "(forceActiveFocus) (item 42)");
        QVERIFY2(fnBlock.contains(QStringLiteral("scrollNoteIntoView")),
                 "focusAndRevealNote() must scroll the revealed note into view (item 42)");
    }

    // Item 13: QQC2 TextField/TextArea and a selectByMouse TextEdit get no
    // I-beam cursor automatically — StyleGuide's "editable/selectable text"
    // must show one via a HoverHandler{cursorShape: Qt.IBeamCursor}. Every
    // TextField/TextArea/TextEdit in MainPanel.qml is editable or
    // selectByMouse, so the invariant is: at least one IBeam handler per
    // such control. No QQuickView here to inspect the live cursor, so this
    // is a source-scan guard — a newly-added text control with no I-beam
    // trips it (the control count rises but the IBeam count doesn't).
    void mainPanel_editableTextControlsHaveIBeamCursor()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const auto countMatches = [&src](const QString &needle) {
            int n = 0;
            int idx = 0;
            while ((idx = src.indexOf(needle, idx)) >= 0) {
                ++n;
                idx += needle.length();
            }
            return n;
        };

        const int controls = countMatches(QStringLiteral("TextField {"))
                             + countMatches(QStringLiteral("TextArea {"))
                             + countMatches(QStringLiteral("TextEdit {"));
        const int ibeams = countMatches(QStringLiteral("Qt.IBeamCursor"));
        QVERIFY2(controls > 0, "MainPanel.qml must declare editable/selectable text controls");
        QVERIFY2(ibeams >= controls,
                 qPrintable(QStringLiteral("every editable/selectable text control in MainPanel.qml must "
                                            "carry an Qt.IBeamCursor HoverHandler (item 13) — found %1 "
                                            "TextField/TextArea/TextEdit but only %2 IBeam handlers")
                                .arg(controls)
                                .arg(ibeams)));
    }

    // Item 46 / Item 20: a real redo stack alongside the existing flat undo
    // — redoLast() exists, is bound to BOTH Ctrl+Y and Ctrl+Shift+Z, and a
    // fresh user edit (pushUndo) clears the redo branch. No QQuickView here
    // to fire the shortcut, so this is a source-scan guard.
    void mainPanel_redoStackAndShortcuts()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("property var redoStack")),
                 "a redoStack must exist alongside undoStack (item 46)");
        QVERIFY2(src.contains(QStringLiteral("function redoLast(")),
                 "redoLast() must exist to re-apply the last undone edit (item 46)");
        QVERIFY2(src.contains(QStringLiteral("function applyHistoryEntry(")),
                 "undo/redo must share one direction-parameterized apply so the two stay symmetric "
                 "and a new entry type is redoable for free (item 46)");

        // Both conventional redo chords must be bound.
        QVERIFY2(src.contains(QStringLiteral("Ctrl+Y")) && src.contains(QStringLiteral("Ctrl+Shift+Z")),
                 "redo must be bound to BOTH Ctrl+Y and Ctrl+Shift+Z (item 46)");
        const int redoShortcutIdx = src.indexOf(QStringLiteral("mainPanel.redoLast()"));
        QVERIFY2(redoShortcutIdx >= 0, "a Shortcut must call redoLast()");

        // Standard semantics: a fresh edit clears the redo branch. pushUndo
        // is the single choke point for every user edit, so the clear lives
        // there.
        const int pushIdx = src.indexOf(QStringLiteral("function pushUndo("));
        QVERIFY2(pushIdx >= 0, "pushUndo() must exist");
        const QString pushBlock = src.mid(pushIdx, 800);
        QVERIFY2(pushBlock.contains(QStringLiteral("redoStack = []")),
                 "pushUndo() must clear redoStack — a fresh user edit invalidates the redo branch "
                 "(standard undo/redo semantics, item 46)");
    }

    // Item 7: find & replace over the transcript, with a regex mode. No
    // QQuickView here to drive the UI, so this source-scan guard asserts the
    // functions, the regex handling, and — critically — that replace routes
    // back through commitTranscriptEdits() (inheriting its per-sentence
    // line-count safety check) rather than a parallel persistence path.
    void mainPanel_findReplaceWithRegex()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("function replaceNext(")),
                 "replaceNext() must exist (item 7)");
        QVERIFY2(src.contains(QStringLiteral("function replaceAll(")),
                 "replaceAll() must exist (item 7)");
        QVERIFY2(src.contains(QStringLiteral("property bool findRegexEnabled")),
                 "a regex-mode toggle (findRegexEnabled) must exist (item 7)");

        // Regex must use the JS/QML-native RegExp engine, guarded so an
        // invalid pattern flags rather than throwing.
        const int reIdx = src.indexOf(QStringLiteral("function buildFindRegex("));
        QVERIFY2(reIdx >= 0, "buildFindRegex() must compile the pattern (item 7)");
        const QString reBlock = src.mid(reIdx, 700);
        QVERIFY2(reBlock.contains(QStringLiteral("new RegExp(")),
                 "regex mode must use JS RegExp (item 7)");
        QVERIFY2(reBlock.contains(QStringLiteral("catch")) && src.contains(QStringLiteral("findReplaceError")),
                 "an invalid regex must set an inline error banner (findReplaceError), not throw (item 7)");

        // Replace must persist through the SAME commit path the editable
        // transcript uses — no parallel writer.
        const int applyIdx = src.indexOf(QStringLiteral("function applyTranscriptReplacement("));
        QVERIFY2(applyIdx >= 0, "applyTranscriptReplacement() must funnel replaces to the commit path");
        const QString applyBlock = src.mid(applyIdx, 300);
        QVERIFY2(applyBlock.contains(QStringLiteral("commitTranscriptEdits(")),
                 "replace must route through commitTranscriptEdits() (its line-count safety check), "
                 "not a parallel persistence path (item 7)");

        // The UI must be reachable (a toggle bool + a shortcut).
        QVERIFY2(src.contains(QStringLiteral("findReplaceExpanded")),
                 "a findReplaceExpanded toggle must gate the find/replace bar (item 7)");
        QVERIFY2(src.contains(QStringLiteral("Ctrl+H")),
                 "Ctrl+H must toggle the find/replace bar (item 7)");
    }

    // Item 41: full keyboard navigation in the focused file panel. No
    // QQuickView in this suite to deliver key events to a TreeView delegate,
    // so these are source-scan guards that the Shortcuts, their focus gating,
    // and the backing functions all exist.

    // Left/Right move keyboard focus into and along the current row's per-row
    // action-button cluster; Enter/Return/Space activate the focused one.
    void sidebar_keyboardFocusesActionButtonCluster()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        // The focus-state property and its nav/activation functions.
        QVERIFY2(src.contains(QStringLiteral("property string actionButtonFocus:")),
                 "Sidebar.qml must hold actionButtonFocus (which action button the keyboard is on)");
        QVERIFY2(src.contains(QStringLiteral("function focusActionButton(direction)")),
                 "focusActionButton() must move focus into/along the action cluster");
        QVERIFY2(src.contains(QStringLiteral("function currentRowActionButtons()")),
                 "currentRowActionButtons() must list the current row's available action buttons");
        QVERIFY2(src.contains(QStringLiteral("function activateFocusedActionButton()")),
                 "activateFocusedActionButton() must replay the focused button's controller call");

        // Left/Right wired as tree-only Shortcuts (search-results view has no
        // action cluster) driving focusActionButton in each direction.
        const int rightIdx = src.indexOf(QStringLiteral("sequence: \"Right\""));
        const int leftIdx = src.indexOf(QStringLiteral("sequence: \"Left\""));
        QVERIFY2(rightIdx >= 0, "Right must be wired as a Shortcut");
        QVERIFY2(leftIdx >= 0, "Left must be wired as a Shortcut");
        const QString rightBlock = src.mid(rightIdx, 200);
        const QString leftBlock = src.mid(leftIdx, 200);
        QVERIFY2(rightBlock.contains(QStringLiteral("enabled: tree.activeFocus"))
                     && !rightBlock.contains(QStringLiteral("searchResultsView.activeFocus")),
                 "Right must be gated on tree focus only (search-results view has no per-row cluster)");
        QVERIFY2(leftBlock.contains(QStringLiteral("enabled: tree.activeFocus"))
                     && !leftBlock.contains(QStringLiteral("searchResultsView.activeFocus")),
                 "Left must be gated on tree focus only");
        QVERIFY2(rightBlock.contains(QStringLiteral("focusActionButton(1)")), "Right must move into/forward");
        QVERIFY2(leftBlock.contains(QStringLiteral("focusActionButton(-1)")), "Left must move backward/out");

        // Enter/Return/Space activate the focused button, only while one is
        // actually focused (so they don't shadow other uses otherwise).
        const int actIdx = src.indexOf(QStringLiteral("sequences: [\"Return\", \"Enter\", \"Space\"]"));
        QVERIFY2(actIdx >= 0, "Return/Enter/Space must be wired to activate the focused action button");
        const QString actBlock = src.mid(actIdx, 220);
        QVERIFY2(actBlock.contains(QStringLiteral("tree.activeFocus"))
                     && actBlock.contains(QStringLiteral("sidebar.actionButtonFocus !== \"\"")),
                 "the activate shortcut must be gated on tree focus AND a button actually being focused");
        QVERIFY2(actBlock.contains(QStringLiteral("activateFocusedActionButton()")),
                 "the activate shortcut must call activateFocusedActionButton()");
    }

    // The current/selected row keeps its action buttons revealed without a
    // mouse hover (so keyboard Left/Right has a visible target); non-selected
    // rows still reveal on hover only, and the focused button shows an outline.
    void sidebar_selectedRowRevealsActionButtons()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        const int idx = src.indexOf(QStringLiteral("property bool rowActionsRevealed:"));
        QVERIFY2(idx >= 0, "treeDelegate must expose rowActionsRevealed for the per-row action cluster");
        const QString block = src.mid(idx, 160);
        QVERIFY2(block.contains(QStringLiteral("rowHover.hovered")),
                 "rowActionsRevealed must still reveal on mouse hover");
        QVERIFY2(block.contains(QStringLiteral("treeDelegate.path === sidebar.currentPath")),
                 "rowActionsRevealed must ALSO reveal for the current/selected row (not just on hover)");

        // The action buttons' opacity must be driven by rowActionsRevealed,
        // not the old hover-only expression.
        QVERIFY2(src.contains(QStringLiteral("opacity: treeDelegate.rowActionsRevealed ? 1 : 0")),
                 "the per-row action buttons must key their opacity off rowActionsRevealed");
        QVERIFY2(!src.contains(QStringLiteral("opacity: rowHover.hovered ? 1 : 0")),
                 "no per-row action button should remain hover-only (item 41 reveals them for the "
                 "selected row too)");

        // The keyboard-focused button gets a HoverBorder outline.
        QVERIFY2(src.contains(QStringLiteral("sidebar.actionButtonFocus === \"transcribe\"")),
                 "the transcribe button must outline when it holds keyboard focus");
        QVERIFY2(src.contains(QStringLiteral("sidebar.actionButtonFocus === \"relabel\"")),
                 "the relabel button must outline when it holds keyboard focus");
        QVERIFY2(src.contains(QStringLiteral("sidebar.actionButtonFocus === \"chapters\"")),
                 "the chapters button must outline when it holds keyboard focus");
    }

    // Shift+Up/Down extend the selection range from the anchor; Ctrl+Up/Down
    // jump to the current group's (parent-folder's) boundary — both gated on
    // the same view-focus condition as plain Up/Down.
    void sidebar_shiftAndCtrlArrowSelection()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("function extendNoteSelection(direction)")),
                 "extendNoteSelection() must back Shift+Up/Down range extension");
        QVERIFY2(src.contains(QStringLiteral("function jumpToGroupBoundary(direction)")),
                 "jumpToGroupBoundary() must back Ctrl+Up/Down group-boundary jumps");
        QVERIFY2(src.contains(QStringLiteral("function parentDir(path)")),
                 "parentDir() must define a note's group (parent folder) for the boundary jump");

        // Shift range must extend from the anchor (lastClickedPath) to the
        // moving cursor (selectionCursorPath), mirroring shift-click.
        const int extIdx = src.indexOf(QStringLiteral("function extendNoteSelection("));
        const QString extBody = src.mid(extIdx, 1200);
        QVERIFY2(extBody.contains(QStringLiteral("sidebar.lastClickedPath"))
                     && extBody.contains(QStringLiteral("sidebar.selectionCursorPath")),
                 "extendNoteSelection must grow the range between the fixed anchor and the moving cursor");
        QVERIFY2(extBody.contains(QStringLiteral("order.slice(lo, hi + 1)")),
                 "extendNoteSelection must select the contiguous anchor..cursor range");

        // Group jump must respect parent-folder boundaries.
        const int jumpIdx = src.indexOf(QStringLiteral("function jumpToGroupBoundary("));
        const QString jumpBody = src.mid(jumpIdx, 1200);
        QVERIFY2(jumpBody.contains(QStringLiteral("sidebar.parentDir(")),
                 "jumpToGroupBoundary must stop at folder boundaries (same parent-folder run)");

        // Both key pairs wired as Shortcuts, gated like plain Up/Down.
        for (const QString &seq : {QStringLiteral("Shift+Up"), QStringLiteral("Shift+Down"),
                                   QStringLiteral("Ctrl+Up"), QStringLiteral("Ctrl+Down")}) {
            const int sIdx = src.indexOf(QStringLiteral("sequence: \"%1\"").arg(seq));
            QVERIFY2(sIdx >= 0, qPrintable(seq + QStringLiteral(" must be wired as a Shortcut")));
            const QString sBlock = src.mid(sIdx, 220);
            QVERIFY2(sBlock.contains(QStringLiteral("tree.activeFocus || searchResultsView.activeFocus")),
                     qPrintable(seq + QStringLiteral(" must be focus-gated exactly like plain Up/Down")));
        }
        const int suIdx = src.indexOf(QStringLiteral("sequence: \"Shift+Up\""));
        QVERIFY2(src.mid(suIdx, 220).contains(QStringLiteral("extendNoteSelection(-1)")),
                 "Shift+Up must extend the selection backward");
        const int cuIdx = src.indexOf(QStringLiteral("sequence: \"Ctrl+Up\""));
        QVERIFY2(src.mid(cuIdx, 220).contains(QStringLiteral("jumpToGroupBoundary(-1)")),
                 "Ctrl+Up must jump to the group's start");
    }

    // Item 27: chapter-generation density is a persisted AppSettings knob
    // (property + defaults entry + debounced save hook), surfaced as a
    // SettingsPopup slider, and every MainPanel generateChapters() call must
    // pass it through. No QQuickView here, so this is a source-scan guard.
    void settings_chapterDensitySliderAndProperty()
    {
        const QString appSettings = readAll(QStringLiteral(SUOP_QML_DIR "/AppSettings.qml"));
        QVERIFY2(!appSettings.isEmpty(), "qml/AppSettings.qml must exist");
        QVERIFY2(appSettings.contains(QStringLiteral("property real chapterDensity")),
                 "chapterDensity must be an AppSettings property (item 27)");
        QVERIFY2(appSettings.contains(QStringLiteral("chapterDensity: 0.5")),
                 "chapterDensity must have a 0.5 default entry in the defaults map (item 27)");
        QVERIFY2(appSettings.contains(QStringLiteral("onChapterDensityChanged()")),
                 "chapterDensity must have a debounced save hook like every other persisted key (item 27)");

        const QString popup = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(!popup.isEmpty(), "qml/SettingsPopup.qml must exist");
        QVERIFY2(popup.contains(QStringLiteral("id: chapterDensitySlider")),
                 "SettingsPopup must expose a chapter-density slider (item 27)");
        QVERIFY2(popup.contains(QStringLiteral("AppSettings.chapterDensity = value")),
                 "the slider must write AppSettings.chapterDensity on move (item 27)");
        QVERIFY2(popup.contains(QStringLiteral("AppSettings.resetProperty(\"chapterDensity\")")),
                 "the slider must have a reset-to-default button (item 27)");

        const QString main = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!main.isEmpty(), "qml/MainPanel.qml must exist");
        // Every generateChapters( call in MainPanel must forward the density.
        int idx = 0;
        int calls = 0;
        while ((idx = main.indexOf(QStringLiteral("generateChapters("), idx)) >= 0) {
            const QString call = main.mid(idx, 160);
            QVERIFY2(call.contains(QStringLiteral("AppSettings.chapterDensity")),
                     "every MainPanel generateChapters() call must pass AppSettings.chapterDensity (item 27)");
            ++calls;
            idx += 1;
        }
        QVERIFY2(calls > 0, "MainPanel.qml must still call generateChapters()");
    }

    // Item 52: MainPanel's open-file icon cluster must offer a standalone
    // "Detect speakers" action that runs diarization on the already-open file
    // without re-transcribing — an Item button wired to diarizeFile(). No
    // QQuickView harness for the delegate, so this is a source-scan guard.
    void mainPanel_detectSpeakersButtonCallsDiarizeFile()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("id: detectSpeakersButton")),
                 "MainPanel must expose a detect-speakers icon button (item 52)");
        // The button must call the standalone diarizeFile() invokable — not a
        // re-transcribe with speakerDetection on.
        int idx = src.indexOf(QStringLiteral("id: detectSpeakersButton"));
        const QString block = src.mid(idx, 1600);
        QVERIFY2(block.contains(QStringLiteral(".diarizeFile(")),
                 "the detect-speakers button must call diarizeFile() (item 52)");
        QVERIFY2(block.contains(QStringLiteral("mainPanel.vaultPath")),
                 "diarizeFile() must be passed the vault path (item 52)");
    }

    // Items 17/35/20: manual chapter add/rename/delete UI wired to the Wave-4
    // backend invokables (addChapter/setChapterTitle/deleteChapter), with the
    // edits pushed onto the undo stack as a chapterEdit entry type routed
    // through applyHistoryEntry(). No QQuickView here, so this is a
    // source-scan guard.
    void mainPanel_chapterAddEditDeleteWiredToBackend()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        // Item 17: an add affordance that inserts at the playhead, and a delete
        // affordance, both calling the backend.
        QVERIFY2(src.contains(QStringLiteral("id: addChapterButton")),
                 "a per-file add-chapter affordance must exist (item 17)");
        QVERIFY2(src.contains(QStringLiteral("function addChapterAtPlayhead(")),
                 "addChapterAtPlayhead() must exist (item 17)");
        QVERIFY2(src.contains(QStringLiteral("player.position / 1000")),
                 "add-chapter must place the marker at the current playback position in seconds (item 17)");
        QVERIFY2(src.contains(QStringLiteral(".addChapter(")),
                 "adding a chapter must call the backend addChapter() invokable (item 17)");
        QVERIFY2(src.contains(QStringLiteral(".deleteChapter(")),
                 "deleting a chapter must call the backend deleteChapter() invokable (item 17)");

        // Item 35: click-to-edit commit routes through setChapterTitle, in a
        // shared commit helper.
        QVERIFY2(src.contains(QStringLiteral("function commitChapterTitle(")),
                 "commitChapterTitle() must exist to commit a rename (item 35)");
        QVERIFY2(src.contains(QStringLiteral(".setChapterTitle(")),
                 "a chapter rename must call the backend setChapterTitle() invokable (item 35)");
        // Both the TOC row and the in-transcript heading must be editable
        // titles.
        QVERIFY2(src.contains(QStringLiteral("id: tocChapterTitle")),
                 "the TOC chapter title must be an editable control (item 35)");
        QVERIFY2(src.contains(QStringLiteral("id: headingTitle")),
                 "the in-transcript chapter heading title must be an editable control (item 35)");

        // Item 20: chapterEdit is a real, dispatched undo entry type.
        QVERIFY2(src.contains(QStringLiteral("type: \"chapterEdit\"")),
                 "chapter edits must be pushed as a chapterEdit undo entry (item 20)");
        const int applyIdx = src.indexOf(QStringLiteral("function applyHistoryEntry("));
        QVERIFY2(applyIdx >= 0, "applyHistoryEntry() must exist");
        const int chapterCaseIdx = src.indexOf(QStringLiteral("case \"chapterEdit\""), applyIdx);
        QVERIFY2(chapterCaseIdx > applyIdx,
                 "applyHistoryEntry() must dispatch the chapterEdit type (item 20)");
        // The three ops must be reversible via the backend invokables, all
        // within the chapterEdit case body.
        const QString chapterCase = src.mid(chapterCaseIdx, 900);
        QVERIFY2(chapterCase.contains(QStringLiteral("setChapterTitle("))
                     && chapterCase.contains(QStringLiteral("addChapter("))
                     && chapterCase.contains(QStringLiteral("deleteChapter(")),
                 "chapterEdit undo/redo must call setChapterTitle/addChapter/deleteChapter to reverse "
                 "rename/delete/add respectively (item 20)");
    }

    // Item 34: chapter titles are a single selectable/editable unit at a time,
    // not per-row free-text selections — a single editingChapterTs state on
    // the MainPanel decides which (if any) title is active, so activating one
    // exits any other. Both title controls gate readOnly/selectByMouse off
    // that state, and clicking away (deselectTextSelections) clears it. No
    // QQuickView here, so this is a source-scan guard.
    void mainPanel_chapterTitleSingleActiveEdit()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("property real editingChapterTs")),
                 "a single editingChapterTs state must track which chapter title is being edited (item 34)");
        // The at-rest chapter title must NOT be freely selectable — readOnly and
        // selectByMouse are gated on the per-title `editing` flag.
        QVERIFY2(src.contains(QStringLiteral("readOnly: !tocChapterTitle.editing"))
                     && src.contains(QStringLiteral("readOnly: !headingTitle.editing")),
                 "chapter titles must be readOnly except when they are the single active edit (item 34)");
        QVERIFY2(src.contains(QStringLiteral("selectByMouse: tocChapterTitle.editing"))
                     && src.contains(QStringLiteral("selectByMouse: headingTitle.editing")),
                 "chapter titles must only be mouse-selectable while actively editing — no lingering "
                 "multi-row selections (item 34)");
        // Clicking away commits+exits the active edit.
        const int deselIdx = src.indexOf(QStringLiteral("function deselectTextSelections("));
        QVERIFY2(deselIdx >= 0, "deselectTextSelections() must exist");
        const QString deselBody = src.mid(deselIdx, 400);
        QVERIFY2(deselBody.contains(QStringLiteral("editingChapterTs = -1")),
                 "clicking a non-interactive region must clear the active chapter edit (item 34)");
    }

    // Item 33: chapters are collapsible in the transcript via a disclosure
    // affordance on the heading, and a collapsed chapter's body sentences are
    // filtered out of displayData (both the flowing text and displaySentences)
    // so the heading stays but its content hides. The whole-TOC collapse
    // (chaptersExpanded) must remain a distinct control. No QQuickView here,
    // so this is a source-scan guard.
    void mainPanel_transcriptChaptersCollapsible()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("property var collapsedChapters")),
                 "a per-chapter collapsed state must exist (item 33)");
        QVERIFY2(src.contains(QStringLiteral("function toggleChapterCollapse(")),
                 "a collapse toggle handler must exist (item 33)");
        // The toggle must be wired on the in-transcript heading row.
        const int headingIdx = src.indexOf(QStringLiteral("id: chapterHeadingRow"));
        QVERIFY2(headingIdx >= 0, "the in-transcript chapter heading row must exist");
        const QString headingBody = src.mid(headingIdx, 3000);
        QVERIFY2(headingBody.contains(QStringLiteral("mainPanel.toggleChapterCollapse(")),
                 "the transcript heading must have a collapse affordance calling toggleChapterCollapse() "
                 "(item 33)");
        // displayData must actually drop collapsed-chapter sentences.
        const int ddIdx = src.indexOf(QStringLiteral("readonly property var displayData"));
        QVERIFY2(ddIdx >= 0, "displayData must exist");
        const QString ddBody = src.mid(ddIdx, 3200);
        QVERIFY2(ddBody.contains(QStringLiteral("collapsed[String(currentChapterT0)]")),
                 "displayData must skip a collapsed chapter's body sentences so they hide from the "
                 "monolithic transcript string and every offset consumer (item 33)");
        // The pre-existing whole-TOC collapse must remain a separate control.
        QVERIFY2(src.contains(QStringLiteral("property bool chaptersExpanded")),
                 "the whole-TOC collapse (chaptersExpanded) must remain distinct from per-chapter "
                 "transcript collapse (item 33)");
    }

    // Item 39 + CLAUDE.md per-file-action-parity: the search-expanded block is
    // the third home (alongside the ribbon's bulk buttons and treeDelegate's
    // per-row cluster) for the transcribe/relabel/chapters run-all actions,
    // here scoped to the CURRENT search/filter results (the filtered/visible
    // set) rather than all-eligible or a multi-select. No QQuickView here to
    // drive the search UI, so this is a source-scan guard that all three
    // actions exist in the search block and are wired to searchResultPaths()'s
    // per-action eligible subsets (not bulkTargetPaths / the whole vault).
    void sidebar_searchMenuHasFilteredRunAllActions()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        // The filtered-set helper the buttons target must exist and derive the
        // set from the search results / filtered tree rows, not a fresh vault
        // walk.
        QVERIFY2(src.contains(QStringLiteral("function searchResultPaths(")),
                 "a searchResultPaths() helper (the filtered/visible result set) must exist (item 39)");
        const int notesIdx = src.indexOf(QStringLiteral("function searchResultNotes("));
        QVERIFY2(notesIdx >= 0, "searchResultNotes() (filtered set + eligibility fields) must exist (item 39)");
        const QString notesBody = src.mid(notesIdx, 900);
        QVERIFY2(notesBody.contains(QStringLiteral("searchResultsList"))
                     && notesBody.contains(QStringLiteral("searchProxy")),
                 "searchResultNotes() must derive from searchResultsList (text mode) and the "
                 "searchProxy tree rows (tag/speaker filter mode), i.e. the actually-visible set");

        // Each action has its own eligibility-narrowed helper.
        QVERIFY2(src.contains(QStringLiteral("function searchTranscribePaths(")),
                 "search-scoped transcribe must narrow to untranscribed results (item 39)");
        QVERIFY2(src.contains(QStringLiteral("function searchRelabelPaths(")),
                 "search-scoped relabel must narrow to transcribed results (item 39)");
        QVERIFY2(src.contains(QStringLiteral("function searchChaptersPaths(")),
                 "search-scoped chapters must narrow to chapter-eligible results (item 39)");

        // All three buttons must live in the search-expanded block (after
        // expandedColumn) and call the same controller entry points the ribbon
        // buttons use, over their eligible subset.
        const int expandedIdx = src.indexOf(QStringLiteral("id: expandedColumn"));
        QVERIFY2(expandedIdx >= 0, "expandedColumn (the search-expanded block) must exist");
        const QString expandedBody = src.mid(expandedIdx);

        const int tIdx = expandedBody.indexOf(QStringLiteral("id: searchTranscribeButton"));
        QVERIFY2(tIdx >= 0, "search-expanded block must have a filtered transcribe run-all button (item 39)");
        QVERIFY2(expandedBody.mid(tIdx, 900).contains(QStringLiteral("searchTranscribePaths()"))
                     && expandedBody.mid(tIdx, 900).contains(QStringLiteral("transcriptionController.transcribeAll(")),
                 "the filtered transcribe button must run transcribeAll() over searchTranscribePaths()");

        const int rIdx = expandedBody.indexOf(QStringLiteral("id: searchRelabelButton"));
        QVERIFY2(rIdx >= 0, "search-expanded block must have a filtered relabel run-all button (item 39)");
        QVERIFY2(expandedBody.mid(rIdx, 900).contains(QStringLiteral("searchRelabelPaths()"))
                     && expandedBody.mid(rIdx, 900).contains(QStringLiteral("transcriptionController.relabelAll(")),
                 "the filtered relabel button must run relabelAll() over searchRelabelPaths()");

        const int cIdx = expandedBody.indexOf(QStringLiteral("id: searchChaptersButton"));
        QVERIFY2(cIdx >= 0, "search-expanded block must have a filtered chapters run-all button (item 39)");
        QVERIFY2(expandedBody.mid(cIdx, 900).contains(QStringLiteral("searchChaptersPaths()"))
                     && expandedBody.mid(cIdx, 900).contains(QStringLiteral("transcriptionController.generateChaptersAll(")),
                 "the filtered chapters button must run generateChaptersAll() over searchChaptersPaths()");

        // Each button must disable itself when its eligible subset is empty
        // (mirrors the ribbon relabel/chapters buttons' enabled state).
        QVERIFY2(expandedBody.mid(tIdx, 900).contains(QStringLiteral("searchTranscribePaths().length > 0"))
                     && expandedBody.mid(rIdx, 900).contains(QStringLiteral("searchRelabelPaths().length > 0"))
                     && expandedBody.mid(cIdx, 900).contains(QStringLiteral("searchChaptersPaths().length > 0")),
                 "each filtered run-all button must disable when its eligible subset of the results is empty");
    }

    // Item 57: the middle-drag autoscroll overlay's ARMED hover-tracking
    // region must be scoped wider than the transcript viewport
    // (transcriptViewportContainer). Qt Quick hover delivery is bounds-scoped
    // per item, so an overlay anchored only to the transcript box froze the
    // free-scroll (gesture 1) the instant the cursor left it. The fix
    // visually reparents the overlay to mainPanel (the whole note page) so
    // its HoverHandler keeps receiving points anywhere in the panel. No
    // QQuickView here, so this is a source-scan guard.
    void mainPanel_middleDragOverlayTracksWiderThanTranscriptViewport()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const int idx = src.indexOf(QStringLiteral("id: middleDragScrollArea"));
        QVERIFY2(idx >= 0, "middleDragScrollArea overlay must exist");
        const QString block = src.mid(idx, 900);
        QVERIFY2(block.contains(QStringLiteral("parent: mainPanel")),
                 "the middle-drag overlay must be reparented to mainPanel so its armed hover "
                 "tracking spans the whole note page, not just transcriptViewportContainer (item 57)");
        QVERIFY2(block.contains(QStringLiteral("anchors.fill: parent")),
                 "the overlay must fill its (now panel-wide) parent so the tracked region is the "
                 "whole page (item 57)");
        // The armed free-scroll must still track via HoverHandler.onPointChanged.
        const int hoverIdx = src.indexOf(QStringLiteral("id: middleDragHover"));
        QVERIFY2(hoverIdx >= 0, "middleDragHover HoverHandler must exist");
        const QString hoverBlock = src.mid(hoverIdx, 500);
        QVERIFY2(hoverBlock.contains(QStringLiteral("onPointChanged"))
                     && hoverBlock.contains(QStringLiteral("currentY")),
                 "the armed HoverHandler must keep updating currentY on point changes so the "
                 "widened tracking region actually drives the autoscroll (item 57)");
    }

    // Item 55: starting a (re)transcription of the OPEN file must, until the
    // first new segment lands, visually dim + hard-disable the stale
    // transcript (no selection/hover/edit) and show a loading overlay on top;
    // once live output begins it reverts so the user watches it stream in. No
    // QQuickView here, so this is a source-scan guard.
    void mainPanel_transcribingBlursDisablesAndShowsLoadingOverlay()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        // Gate: shown only from job-start until the first new segment lands.
        QVERIFY2(src.contains(QStringLiteral("property bool awaitingFirstSegment")),
                 "an awaitingFirstSegment flag (job-start until first live segment) must exist (item 55)");
        QVERIFY2(src.contains(QStringLiteral("readonly property bool showTranscribingOverlay")),
                 "a showTranscribingOverlay gate must drive the loading state (item 55)");
        QVERIFY2(src.contains(QStringLiteral("awaitingFirstSegment = false")),
                 "the overlay must clear once the new transcript starts being written (item 55)");

        // The stale transcript is dimmed AND non-interactive while shown.
        const int svIdx = src.indexOf(QStringLiteral("id: transcriptScrollView"));
        QVERIFY2(svIdx >= 0, "transcriptScrollView must exist");
        const QString svBlock = src.mid(svIdx, 1600);
        QVERIFY2(svBlock.contains(QStringLiteral("enabled: !mainPanel.showTranscribingOverlay")),
                 "the transcript must be hard-disabled (no selection/hover/edit) while transcribing "
                 "hasn't produced new output yet (item 55)");
        QVERIFY2(svBlock.contains(QStringLiteral("opacity: mainPanel.showTranscribingOverlay")),
                 "the stale transcript must be dimmed while the loading overlay is shown (item 55)");

        // A loading overlay (spinner + label) gated on the same flag.
        const int ovIdx = src.indexOf(QStringLiteral("id: transcribingOverlay"));
        QVERIFY2(ovIdx >= 0, "a transcribingOverlay must exist (item 55)");
        const QString ovBlock = src.mid(ovIdx, 900);
        QVERIFY2(ovBlock.contains(QStringLiteral("visible: mainPanel.showTranscribingOverlay")),
                 "the loading overlay must be gated on showTranscribingOverlay (item 55)");
        QVERIFY2(ovBlock.contains(QStringLiteral("BusyIndicator")),
                 "the loading overlay must show a busy/spinner indicator (item 55)");
    }

    // Item 32: on a file's FIRST transcription the regenerate
    // label/prelude/chapters buttons gate on hasStoredMetadata, which flips
    // true when refreshMetadata() reads the frontmatter written at job start.
    // onProgressChanged can fire during model-load BEFORE that flush, so the
    // definitely-post-frontmatter live-content edge (onLiveUpdateTickChanged)
    // must ALSO refresh metadata for the open file — otherwise the buttons
    // could stay hidden until the next tick or a full reopen. Source-scan
    // guard for that invariant.
    void mainPanel_liveTickRefreshesMetadataForOpenFile()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const int idx = src.indexOf(QStringLiteral("function onLiveUpdateTickChanged("));
        QVERIFY2(idx >= 0, "onLiveUpdateTickChanged handler must exist");
        const QString block = src.mid(idx, 1400);
        QVERIFY2(block.contains(QStringLiteral("refreshMetadata()")),
                 "the live-content tick must refresh metadata for the open file so hasStoredMetadata "
                 "flips true (regenerate buttons appear) on a file's first transcription (item 32)");
    }

    // Items 24/53 + CLAUDE.md per-file-action-parity: "run all jobs"
    // (transcribe->label->chapters) must exist in ALL required button sets —
    // Sidebar's ribbon (bulk), Sidebar's per-row hover cluster, Sidebar's
    // search-scoped run-all group, and MainPanel's icon cluster (open file) —
    // each wired to the runAllJobs()/runAllJobsForPaths() controller
    // invokables. No QQuickView here, so this is a source-scan guard.
    void sidebar_runAllJobsActionParityAcrossAllSites()
    {
        const QString sidebar = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!sidebar.isEmpty(), "qml/Sidebar.qml must exist");
        const QString mainPanel = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!mainPanel.isEmpty(), "qml/MainPanel.qml must exist");

        // (1) Ribbon bulk run-all button, next to transcribeAll/relabelAll/
        // chaptersAll, narrowing via bulkTargetPaths() and running the bulk
        // controller entry point.
        const int ribbonIdx = sidebar.indexOf(QStringLiteral("id: runAllButton"));
        QVERIFY2(ribbonIdx >= 0, "Sidebar ribbon must have a bulk runAllButton (items 24/53)");
        const QString ribbonBlock = sidebar.mid(ribbonIdx, 1200);
        QVERIFY2(ribbonBlock.contains(QStringLiteral("bulkTargetPaths(sidebar.untranscribedPaths)")),
                 "the ribbon run-all button must narrow to the selection via bulkTargetPaths()");
        QVERIFY2(ribbonBlock.contains(QStringLiteral("transcriptionController.runAllJobsForPaths(")),
                 "the ribbon run-all button must call the bulk runAllJobsForPaths() invokable");

        // (2) Per-row hover-cluster run-all button in treeDelegate.
        const int rowIdx = sidebar.indexOf(QStringLiteral("id: rowRunAllButton"));
        QVERIFY2(rowIdx >= 0, "treeDelegate must have a per-row rowRunAllButton (items 24/53)");
        QVERIFY2(sidebar.mid(rowIdx, 1200).contains(QStringLiteral("transcriptionController.runAllJobs(")),
                 "the per-row run-all button must call runAllJobs() for that one file");

        // Keyboard-nav cluster reachability: "runAll" must be a focusable
        // action in the cluster order and dispatched on activation.
        QVERIFY2(sidebar.contains(QStringLiteral("list.push(\"runAll\")")),
                 "runAll must be part of currentRowActionButtons() so Left/Right can focus it (items 24/53)");
        QVERIFY2(sidebar.contains(QStringLiteral("sidebar.actionButtonFocus === \"runAll\"")),
                 "activating a keyboard-focused runAll button must dispatch runAllJobs() (items 24/53)");

        // (3) Search-scoped run-all button + its eligibility helper, inside the
        // search-expanded block.
        QVERIFY2(sidebar.contains(QStringLiteral("function searchRunAllPaths(")),
                 "a search-scoped searchRunAllPaths() eligibility helper must exist (items 24/53)");
        const int expandedIdx = sidebar.indexOf(QStringLiteral("id: expandedColumn"));
        QVERIFY2(expandedIdx >= 0, "expandedColumn (the search-expanded block) must exist");
        const QString expandedBody = sidebar.mid(expandedIdx);
        const int searchIdx = expandedBody.indexOf(QStringLiteral("id: searchRunAllButton"));
        QVERIFY2(searchIdx >= 0, "search-expanded block must have a filtered searchRunAllButton (items 24/53)");
        QVERIFY2(expandedBody.mid(searchIdx, 1200).contains(QStringLiteral("searchRunAllPaths()"))
                     && expandedBody.mid(searchIdx, 1200).contains(
                            QStringLiteral("transcriptionController.runAllJobsForPaths(")),
                 "the filtered run-all button must run runAllJobsForPaths() over searchRunAllPaths()");

        // (4) MainPanel icon cluster run-all button — per the user's spec,
        // shown only when there's no transcription yet (untranscribed open
        // file), and wired to runAllJobs() for that file.
        const int mpIdx = mainPanel.indexOf(QStringLiteral("id: runAllIconButton"));
        QVERIFY2(mpIdx >= 0, "MainPanel must have a runAllIconButton (items 24/53)");
        const QString mpBlock = mainPanel.mid(mpIdx, 1500);
        QVERIFY2(mpBlock.contains(QStringLiteral("!mainPanel.hasStoredMetadata")),
                 "the MainPanel run-all button must appear only when there is no transcription yet "
                 "(untranscribed open file) per the user's spec (items 24/53)");
        QVERIFY2(mpBlock.contains(QStringLiteral("transcriptionController.runAllJobs(")),
                 "the MainPanel run-all button must call runAllJobs() for the open file");

        // Every run-all call site (both files) must forward AppSettings.chapterDensity
        // for the chapters step, mirroring generateChapters()'s density plumbing.
        for (const QString &src : {sidebar, mainPanel}) {
            int idx = 0;
            // Only real invocations (the "transcriptionController." prefix) —
            // prose mentions of runAllJobs() in comments have no such prefix.
            // Bound the call by its own closing paren (these arg lists contain
            // no nested parens), not a fixed char window: deep continuation-
            // line alignment can push the last arg well past any fixed span.
            while ((idx = src.indexOf(QStringLiteral("transcriptionController.runAllJobs"), idx)) >= 0) {
                const int close = src.indexOf(QLatin1Char(')'), idx);
                QVERIFY(close > idx);
                const QString call = src.mid(idx, close - idx);
                QVERIFY2(call.contains(QStringLiteral("AppSettings.chapterDensity")),
                         "every runAllJobs()/runAllJobsForPaths() QML call site must pass "
                         "AppSettings.chapterDensity for the chapters step (items 24/53)");
                idx = close;
            }
        }
    }

    // Item 2 (delete transcript + delete file): the two destructive file
    // actions must reach the user via the Sidebar context menu AND the
    // MainPanel open-file icon cluster (CLAUDE.md per-file-action parity; the
    // Sidebar per-row site is satisfied by its right-click context menu, the
    // natural home alongside "Delete folder"). A bulk ribbon variant is
    // deliberately NOT added — a one-click bulk "delete file" over a
    // multi-selection is too dangerous, and per-file + open-file are the
    // parity sites that matter for a destructive action. No QQuickView here,
    // so this is a source-scan guard that the wiring exists.
    void delete_transcriptAndFile_wiredInSidebarAndMainPanel()
    {
        const QString sidebar = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!sidebar.isEmpty(), "qml/Sidebar.qml must exist");

        // Context-menu entries for both actions.
        QVERIFY2(sidebar.contains(QStringLiteral("id: ctxDeleteTranscriptRow")),
                 "Sidebar context menu must offer a 'Delete transcript' entry for transcribed files");
        QVERIFY2(sidebar.contains(QStringLiteral("id: ctxDeleteFileRow")),
                 "Sidebar context menu must offer a 'Delete file' entry for note rows");
        // "Delete transcript" is gated to a transcribed AUDIO row (status 2).
        QVERIFY2(sidebar.contains(QStringLiteral("visible: sidebar.ctxIsAudio && sidebar.ctxStatus === 2")),
                 "'Delete transcript' must only show for a transcribed audio row (an orphaned .md gets "
                 "'Delete file' instead)");
        // Both route through the model's invokables (via a confirm popup).
        QVERIFY2(sidebar.contains(QStringLiteral(".deleteTranscript(")),
                 "Sidebar must call VaultTreeModel.deleteTranscript()");
        QVERIFY2(sidebar.contains(QStringLiteral(".deleteFile(")),
                 "Sidebar must call VaultTreeModel.deleteFile()");
        // Destructive => must be confirmed first.
        QVERIFY2(sidebar.contains(QStringLiteral("id: fileDeleteConfirmPopup")),
                 "a destructive file/transcript delete must be confirmed first");

        const QString mainPanel = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!mainPanel.isEmpty(), "qml/MainPanel.qml must exist");

        QVERIFY2(mainPanel.contains(QStringLiteral("id: deleteTranscriptButton")),
                 "MainPanel icon cluster must have a 'Delete transcript' action for the open file");
        QVERIFY2(mainPanel.contains(QStringLiteral("id: deleteFileButton")),
                 "MainPanel icon cluster must have a 'Delete file' action for the open file");
        QVERIFY2(mainPanel.contains(QStringLiteral(".deleteTranscript("))
                     && mainPanel.contains(QStringLiteral(".deleteFile(")),
                 "MainPanel must call VaultTreeModel.deleteTranscript()/deleteFile()");
        QVERIFY2(mainPanel.contains(QStringLiteral("id: deleteFileConfirmPopup")),
                 "MainPanel's delete actions must be confirmed first");
        // When the OPEN file is deleted, the view must clear/refresh — driven
        // off the model's transcriptDeleted/fileDeleted signals so it works
        // whether the delete was triggered here or from the Sidebar.
        QVERIFY2(mainPanel.contains(QStringLiteral("onTranscriptDeleted"))
                     && mainPanel.contains(QStringLiteral("onFileDeleted")),
                 "MainPanel must react to VaultTreeModel.transcriptDeleted/fileDeleted so the open file's "
                 "view refreshes (revert) or clears (deleted) gracefully");
    }

    // Item 27 + chapterDensity plumbing: every generateChapters()/
    // generateChaptersAll() QML call site in Sidebar.qml must forward
    // AppSettings.chapterDensity (matching MainPanel's call site), not rely on
    // the C++ default 0.5 — otherwise the density slider silently does nothing
    // for chapters kicked off from the sidebar.
    void sidebar_generateChaptersCallsPassChapterDensity()
    {
        const QString sidebar = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!sidebar.isEmpty(), "qml/Sidebar.qml must exist");

        int idx = 0;
        int found = 0;
        // Only real invocations (the "transcriptionController." prefix) — prose
        // mentions in comments have no such prefix. Matches both
        // generateChapters( and generateChaptersAll(. Bound each call by its
        // own closing paren (these arg lists contain no nested parens).
        while ((idx = sidebar.indexOf(QStringLiteral("transcriptionController.generateChapters"), idx)) >= 0) {
            const int close = sidebar.indexOf(QLatin1Char(')'), idx);
            QVERIFY(close > idx);
            const QString call = sidebar.mid(idx, close - idx);
            QVERIFY2(call.contains(QStringLiteral("AppSettings.chapterDensity")),
                     "every generateChapters()/generateChaptersAll() Sidebar call site must pass "
                     "AppSettings.chapterDensity (item 27) so the density slider takes effect");
            ++found;
            idx = close;
        }
        QVERIFY2(found >= 4,
                 "expected all four Sidebar generateChapters call sites (keyboard-nav, ribbon "
                 "chaptersAll, search chapters, per-row rowChapters) to be present");
    }

    // Item 52 (CLAUDE.md per-file-action parity): the standalone "detect
    // speakers" (diarization) action must exist in all three Sidebar sites
    // mirroring MainPanel's detectSpeakersButton — the ribbon bulk button, the
    // per-row hover cluster, and the search-scoped run group — plus be
    // reachable via keyboard-nav. Ribbon/search bulk sites call
    // diarizeFilesForPaths(); the per-row/keyboard sites call diarizeFile().
    void sidebar_detectSpeakersActionParityAcrossAllSites()
    {
        const QString sidebar = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!sidebar.isEmpty(), "qml/Sidebar.qml must exist");

        // (1) Ribbon bulk detect-speakers button — over the transcribed set
        // (relabel-all's eligibility, since diarization needs a transcript).
        const int ribbonIdx = sidebar.indexOf(QStringLiteral("id: detectSpeakersAllButton"));
        QVERIFY2(ribbonIdx >= 0, "Sidebar ribbon must have a bulk detectSpeakersAllButton (item 52)");
        const QString ribbonBlock = sidebar.mid(ribbonIdx, 1200);
        QVERIFY2(ribbonBlock.contains(QStringLiteral("bulkTargetPaths(sidebar.transcribedPaths)")),
                 "the ribbon detect-speakers button must operate over the TRANSCRIBED set "
                 "(diarization needs an existing transcript)");
        QVERIFY2(ribbonBlock.contains(QStringLiteral("transcriptionController.diarizeFilesForPaths(")),
                 "the ribbon detect-speakers button must call the bulk diarizeFilesForPaths() invokable");

        // (2) Per-row hover-cluster detect-speakers button in treeDelegate.
        const int rowIdx = sidebar.indexOf(QStringLiteral("id: rowDetectSpeakersButton"));
        QVERIFY2(rowIdx >= 0, "treeDelegate must have a per-row rowDetectSpeakersButton (item 52)");
        const QString rowBlock = sidebar.mid(rowIdx, 1200);
        QVERIFY2(rowBlock.contains(QStringLiteral("treeDelegate.relabelEligible")),
                 "the per-row detect-speakers button must be gated on a transcribed audio row");
        QVERIFY2(rowBlock.contains(QStringLiteral("transcriptionController.diarizeFile(")),
                 "the per-row detect-speakers button must call diarizeFile() for that one file");

        // Keyboard-nav cluster reachability: "detectSpeakers" must be a
        // focusable action in the cluster order and dispatched on activation.
        QVERIFY2(sidebar.contains(QStringLiteral("list.push(\"detectSpeakers\")")),
                 "detectSpeakers must be part of currentRowActionButtons() so Left/Right can focus it (item 52)");
        QVERIFY2(sidebar.contains(QStringLiteral("sidebar.actionButtonFocus === \"detectSpeakers\"")),
                 "activating a keyboard-focused detectSpeakers button must dispatch diarizeFile() (item 52)");

        // (3) Search-scoped detect-speakers button + its eligibility helper.
        QVERIFY2(sidebar.contains(QStringLiteral("function searchDetectSpeakersPaths(")),
                 "a search-scoped searchDetectSpeakersPaths() eligibility helper must exist (item 52)");
        const int expandedIdx = sidebar.indexOf(QStringLiteral("id: expandedColumn"));
        QVERIFY2(expandedIdx >= 0, "expandedColumn (the search-expanded block) must exist");
        const QString expandedBody = sidebar.mid(expandedIdx);
        const int searchIdx = expandedBody.indexOf(QStringLiteral("id: searchDetectSpeakersButton"));
        QVERIFY2(searchIdx >= 0, "search-expanded block must have a filtered searchDetectSpeakersButton (item 52)");
        const QString searchBlock = expandedBody.mid(searchIdx, 1200);
        QVERIFY2(searchBlock.contains(QStringLiteral("searchDetectSpeakersPaths()"))
                     && searchBlock.contains(QStringLiteral("transcriptionController.diarizeFilesForPaths(")),
                 "the filtered detect-speakers button must run diarizeFilesForPaths() over "
                 "searchDetectSpeakersPaths()");
    }

    // Regression: "regenerate chapters" was reachable from all three
    // per-file-action-parity sites, but every one of them gated on
    // relabelEligible (isAudio required), so an orphaned .md (audio deleted,
    // text/transcript still present) could never reach it — even though
    // TranscriptionController::generateChaptersJob() only ever reads the
    // .md's existing frontmatter/body and never touches audio (same as
    // relabelJob(), which MainPanel's regenerateLabelButton already exercises
    // for orphaned .md via orphanedMdPath/targetUrl). searchChaptersPaths()
    // and currentRowActionButtons() already expected `relabelEligible ||
    // isOrphanedMd` for chapters eligibility (see their existing bodies
    // below) — the ribbon/per-row/MainPanel sites just hadn't been wired to
    // match. No QQuickView here, so this is a source-scan guard.
    void generateChapters_orphanedMdEligibleAcrossAllSites()
    {
        const QString sidebar = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!sidebar.isEmpty(), "qml/Sidebar.qml must exist");
        const QString mainPanel = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!mainPanel.isEmpty(), "qml/MainPanel.qml must exist");

        // treeDelegate's own chaptersEligible must be a strict superset of
        // relabelEligible, adding isOrphanedMd — matching the inline
        // `relabelEligible || isOrphanedMd` expression currentRowActionButtons()
        // and the search-results delegate already use for this same purpose.
        const int eligIdx = sidebar.indexOf(QStringLiteral("readonly property bool chaptersEligible"));
        QVERIFY2(eligIdx >= 0, "treeDelegate must expose a chaptersEligible property");
        const QString eligLine = sidebar.mid(eligIdx, 200);
        QVERIFY2(eligLine.contains(QStringLiteral("treeDelegate.relabelEligible"))
                     && eligLine.contains(QStringLiteral("treeDelegate.isOrphanedMd")),
                 "chaptersEligible must be relabelEligible || isOrphanedMd, not relabelEligible alone");

        // (1) Ribbon bulk chaptersAllButton must run over an orphaned-inclusive
        // set (chaptersPaths), not the audio-only transcribedPaths.
        const int ribbonIdx = sidebar.indexOf(QStringLiteral("id: chaptersAllButton"));
        QVERIFY2(ribbonIdx >= 0, "Sidebar ribbon must have a bulk chaptersAllButton");
        const QString ribbonBlock = sidebar.mid(ribbonIdx, 1200);
        QVERIFY2(ribbonBlock.contains(QStringLiteral("sidebar.chaptersPaths"))
                     && !ribbonBlock.contains(QStringLiteral("sidebar.transcribedPaths")),
                 "the ribbon chapters button must target chaptersPaths (orphaned-inclusive), not "
                 "transcribedPaths (audio-only)");

        // (2) Per-row hover-cluster rowChaptersButton must gate on
        // chaptersEligible, not relabelEligible.
        const int rowIdx = sidebar.indexOf(QStringLiteral("id: rowChaptersButton"));
        QVERIFY2(rowIdx >= 0, "treeDelegate must have a per-row rowChaptersButton");
        const QString rowBlock = sidebar.mid(rowIdx, 800);
        QVERIFY2(rowBlock.contains(QStringLiteral("treeDelegate.chaptersEligible")),
                 "the per-row chapters button must be gated on chaptersEligible so orphaned .md rows "
                 "can reach it");

        // (3) MainPanel's generateChaptersButton must support orphaned .md the
        // same way regenerateLabelButton already does: visible whenever
        // orphanedMdPath is set (not excluded), and dispatching against a
        // targetUrl that resolves to the orphaned path rather than
        // player.source.
        const int mpIdx = mainPanel.indexOf(QStringLiteral("id: generateChaptersButton"));
        QVERIFY2(mpIdx >= 0, "MainPanel must have a generateChaptersButton");
        const QString mpBlock = mainPanel.mid(mpIdx, 1000);
        QVERIFY2(mpBlock.contains(QStringLiteral("mainPanel.orphanedMdPath !== \"\"")),
                 "generateChaptersButton must NOT be hidden whenever an orphaned .md is open");
        QVERIFY2(mpBlock.contains(QStringLiteral("generateChaptersButton.targetUrl")),
                 "generateChaptersButton must dispatch via a targetUrl that resolves to orphanedMdPath "
                 "when open, mirroring regenerateLabelButton's targetUrl");

        // refreshPreludeAndChapters() must be able to populate mainPanel.chapters
        // for an orphaned .md too, so a freshly-generated chapter list actually
        // becomes visible right after the job's onFinished fires.
        const int refreshIdx = mainPanel.indexOf(QStringLiteral("function refreshPreludeAndChapters()"));
        QVERIFY2(refreshIdx >= 0, "refreshPreludeAndChapters() must exist");
        const QString refreshBlock = mainPanel.mid(refreshIdx, 700);
        QVERIFY2(refreshBlock.contains(QStringLiteral("mainPanel.orphanedMdPath !== \"\"")),
                 "refreshPreludeAndChapters() must read chapters via orphanedMdPath when open, not "
                 "only via player.source");

        // The chapter list itself must not be force-hidden for orphaned .md
        // (it only ever needs mainPanel.chapters, which the above now
        // populates for orphaned files too).
        const int listIdx = mainPanel.indexOf(QStringLiteral("id: chapterListColumn"));
        QVERIFY2(listIdx >= 0, "chapterListColumn must exist");
        const QString listBlock = mainPanel.mid(listIdx, 200);
        QVERIFY2(!listBlock.contains(QStringLiteral("mainPanel.orphanedMdPath ===")),
                 "chapterListColumn must not be gated shut for orphaned .md files");
    }

};

QTEST_GUILESS_MAIN(TstStyleGuide)
#include "tst_styleguide.moc"
