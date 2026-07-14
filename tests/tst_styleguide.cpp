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
        QVERIFY2(filenameTextBlock.contains(QStringLiteral("elide: Text.ElideRight")),
                 "treeDelegate's filename Text must elide — otherwise a long filename overlaps the "
                 "per-row icon cluster (rowTranscribeButton/groupMergeButton/etc) instead of stopping "
                 "short of it");
        QVERIFY2(filenameTextBlock.contains(QStringLiteral("clip: true")),
                 "treeDelegate's filename Text must clip — elide is documented elsewhere in this file "
                 "as unreliable together with Text.StyledText (the search-highlight case), so clip is "
                 "the backstop that keeps overflow from painting over the icon cluster even then");
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
        QVERIFY(src.contains(QStringLiteral("border.color: \"white\"")));
        QVERIFY2(src.contains(QStringLiteral("Behavior on opacity")),
                 "hover outline must fade, not pop (StyleGuide.md)");
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
};

QTEST_GUILESS_MAIN(TstStyleGuide)
#include "tst_styleguide.moc"
