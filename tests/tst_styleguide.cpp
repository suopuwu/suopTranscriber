#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QSet>

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

    // The source of the QML element enclosing `anchor` (typically its
    // "id: foo" line), brace-matched from that element's opening brace to its
    // close.
    //
    // Prefer this to a fixed-width src.mid(anchorIdx, N) window: N rots the
    // moment the block grows past it, and it rots SILENTLY IN THE WRONG
    // DIRECTION — the assertion stops finding what it's looking for and reports
    // a missing feature that's actually right there, so the failure blames the
    // code instead of the test. That's already happened twice on this file's
    // rowRenameField guard. Returns an empty string if `anchor` isn't found.
    static QString enclosingBlock(const QString &src, const QString &anchor)
    {
        const int anchorIdx = src.indexOf(anchor);
        if (anchorIdx < 0)
            return QString();
        const int open = src.lastIndexOf(QLatin1Char('{'), anchorIdx);
        if (open < 0)
            return QString();
        int depth = 0;
        for (int i = open; i < src.size(); ++i) {
            if (src.at(i) == QLatin1Char('{')) {
                ++depth;
            } else if (src.at(i) == QLatin1Char('}')) {
                if (--depth == 0)
                    return src.mid(open, i - open + 1);
            }
        }
        return src.mid(open);
    }

    // `src` with its // line comments removed, so a "does the code do X" scan
    // can't be satisfied (or defeated) by prose merely MENTIONING X.
    //
    // This bites both ways and both have happened here: a guard asserting a
    // call is ABSENT fails the moment someone documents why it's absent (the
    // comment contains the call text), and a guard asserting a call is PRESENT
    // passes on a comment alone. Skips '//' inside double-quoted strings so
    // literals like "file://" survive intact.
    static QString stripComments(const QString &src)
    {
        QString out;
        out.reserve(src.size());
        bool inString = false;
        for (int i = 0; i < src.size(); ++i) {
            const QChar c = src.at(i);
            if (inString) {
                if (c == QLatin1Char('\\') && i + 1 < src.size()) {
                    out += c;
                    out += src.at(++i); // escaped char, never ends the string
                    continue;
                }
                if (c == QLatin1Char('"'))
                    inString = false;
                out += c;
                continue;
            }
            if (c == QLatin1Char('"')) {
                inString = true;
                out += c;
                continue;
            }
            if (c == QLatin1Char('/') && i + 1 < src.size() && src.at(i + 1) == QLatin1Char('/')) {
                while (i < src.size() && src.at(i) != QLatin1Char('\n'))
                    ++i;
                if (i < src.size())
                    out += QLatin1Char('\n'); // keep line structure intact
                continue;
            }
            out += c;
        }
        return out;
    }

    // The body of the JS function whose declaration is `signature`, brace-
    // matched FORWARD from the brace that opens it.
    //
    // enclosingBlock() above must not be used for this: it scans BACKWARD for
    // the nearest preceding '{', which for a `function foo() {` anchor is
    // whatever block came before — for a function that isn't the first in its
    // parent, that's the PREVIOUS SIBLING function's brace, so it silently
    // returns the wrong function's body. That's not hypothetical: it made
    // sidebar_jobStatusTicksDoNotRebuildTreeOrdering() assert against
    // onFinished()'s body while claiming to test onJobStatusesChanged(), and
    // it's the same silently-wrong-direction rot enclosingBlock()'s own doc
    // comment warns about. A function's brace comes AFTER its name, so search
    // forward. Returns an empty string if `signature` isn't found.
    static QString functionBody(const QString &src, const QString &signature)
    {
        const int anchorIdx = src.indexOf(signature);
        if (anchorIdx < 0)
            return QString();
        const int open = src.indexOf(QLatin1Char('{'), anchorIdx);
        if (open < 0)
            return QString();
        int depth = 0;
        for (int i = open; i < src.size(); ++i) {
            if (src.at(i) == QLatin1Char('{')) {
                ++depth;
            } else if (src.at(i) == QLatin1Char('}')) {
                if (--depth == 0)
                    return src.mid(open, i - open + 1);
            }
        }
        return src.mid(open);
    }

    // The functionBody() equivalent for a binding whose value is a single CALL
    // rather than a braced block — e.g.
    // `readonly property var displayData: TranscriptDisplayBuilder.build(...)`.
    // There is no brace for functionBody()/enclosingBlock() to match, and a
    // fixed-width src.mid() window is exactly the silently-rotting pattern
    // this file's other helpers exist to avoid, so balance the call's own
    // parentheses from the anchor forward. Returns empty if `anchor` isn't
    // found or the parens never balance.
    static QString bindingCall(const QString &src, const QString &anchor)
    {
        const int anchorIdx = src.indexOf(anchor);
        if (anchorIdx < 0)
            return QString();
        int depth = 0;
        for (int i = anchorIdx; i < src.size(); ++i) {
            if (src.at(i) == QLatin1Char('(')) {
                ++depth;
            } else if (src.at(i) == QLatin1Char(')')) {
                if (--depth == 0)
                    return src.mid(anchorIdx, i - anchorIdx + 1);
            }
        }
        return QString();
    }

    // Sidebar tree-row code now lives in TreeRowDelegate.qml; reverse its
    // self-shadow rename so this file's existing `sidebar.`/`tree.` guard
    // literals still match. Concatenate with Sidebar.qml so guards that also
    // check non-delegate Sidebar code keep working.
    static QString sidebarTreeSrc()
    {
        QString delegate = readAll(QStringLiteral(SUOP_QML_DIR "/TreeRowDelegate.qml"));
        delegate.replace(QStringLiteral("sidebarRoot."), QStringLiteral("sidebar."));
        delegate.replace(QStringLiteral("treeRoot."), QStringLiteral("tree."));
        // The flat search-results delegate now lives in SearchResultDelegate.qml;
        // reverse its sidebarRoot.->sidebar. / listView.->searchResultsView. rename
        // so this file's existing guard literals still match.
        QString searchDelegate = readAll(QStringLiteral(SUOP_QML_DIR "/SearchResultDelegate.qml"));
        searchDelegate.replace(QStringLiteral("sidebarRoot."), QStringLiteral("sidebar."));
        searchDelegate.replace(QStringLiteral("listView."), QStringLiteral("searchResultsView."));
        // The ribbon's bulk-action button flow now lives in SidebarRibbon.qml;
        // same sidebarRoot.->sidebar. reversal so guard literals still match.
        QString ribbon = readAll(QStringLiteral(SUOP_QML_DIR "/SidebarRibbon.qml"));
        ribbon.replace(QStringLiteral("sidebarRoot."), QStringLiteral("sidebar."));
        return readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")) + delegate + searchDelegate + ribbon;
    }

    // MainPanel seek bar + chapter heading now live in SeekBar.qml /
    // ChapterHeadingRow.qml; reverse their `panel.`->`mainPanel.` /
    // `txArea.`->`transcriptArea.` rename. Apply the replace ONLY to the
    // extracted files (MainPanel.qml already has real `mainPanel.`, and
    // `mainPanel.` CONTAINS `panel.` — replacing on the whole concat would
    // corrupt it to `mainmainPanel.`).
    static QString mainPanelSrc()
    {
        QString seek = readAll(QStringLiteral(SUOP_QML_DIR "/SeekBar.qml"));
        seek.replace(QStringLiteral("panel."), QStringLiteral("mainPanel."));
        QString chapter = readAll(QStringLiteral(SUOP_QML_DIR "/ChapterHeadingRow.qml"));
        chapter.replace(QStringLiteral("panel."), QStringLiteral("mainPanel."));
        chapter.replace(QStringLiteral("txArea."), QStringLiteral("transcriptArea."));
        QString prelude = readAll(QStringLiteral(SUOP_QML_DIR "/PreludeEditor.qml"));
        prelude.replace(QStringLiteral("panel."), QStringLiteral("mainPanel."));
        QString toc = readAll(QStringLiteral(SUOP_QML_DIR "/ChapterTocList.qml"));
        toc.replace(QStringLiteral("panel."), QStringLiteral("mainPanel."));
        QString findBar = readAll(QStringLiteral(SUOP_QML_DIR "/SearchReplaceBar.qml"));
        findBar.replace(QStringLiteral("panel."), QStringLiteral("mainPanel."));
        QString replaceRow = readAll(QStringLiteral(SUOP_QML_DIR "/ReplaceRow.qml"));
        replaceRow.replace(QStringLiteral("panel."), QStringLiteral("mainPanel."));
        return readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")) + seek + chapter + prelude + toc
                + findBar + replaceRow;
    }

private slots:
    // Guards functionBody() itself — it is the thing standing between
    // "onJobStatusesChanged is clean" and a test that silently asserts against
    // the wrong function's body. Pins that it returns THIS function's body and
    // not a neighbour's, using two adjacent Sidebar.qml handlers whose bodies
    // are known to differ (onFinished calls refreshTreeOrdering(),
    // onJobStatusesChanged deliberately does not).
    void functionBodyHelper_selectsTheRightFunction()
    {
        // Stripped first, as the real guard does — a stray brace inside a
        // comment would otherwise throw off the brace matching.
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        const QString second = functionBody(src, QStringLiteral("function onJobStatusesChanged()"));
        QVERIFY2(!second.isEmpty(), "onJobStatusesChanged must exist");
        QVERIFY2(second.contains(QStringLiteral("jobStatusRevision")),
                 "functionBody() must return onJobStatusesChanged's OWN body");
        QVERIFY2(!second.contains(QStringLiteral("refreshTreeOrdering")),
                 "functionBody() must not bleed into the preceding onFinished's body");

        // Prefix, not the full parameter list — the signature gained a 4th
        // `structural` param (queue item 021) and an exact-literal lookup
        // silently stopped finding the function at all.
        const QString first = functionBody(src, QStringLiteral("function onFinished("));
        QVERIFY2(!first.isEmpty(), "onFinished must exist");
        QVERIFY2(first.contains(QStringLiteral("refreshTreeOrdering")),
                 "functionBody() must return onFinished's own body");
        QVERIFY2(!first.contains(QStringLiteral("jobStatusRevision")),
                 "functionBody() must stop at onFinished's close, not run on into its sibling");
    }

    // Hazard guard: searchProxy.fileSortKeys must be assigned in EXACTLY ONE
    // place, and that place is the tail of refreshTreeOrdering().
    //
    // Writing it re-enters VaultSearchProxyModel's invalidate, which once
    // segfaulted by walking freed Nodes when the write landed in the middle of
    // a source-model reset (fixed by deferring the invalidate inside the
    // proxy — see tst_vaultsearchproxymodel's reset tests). The fix holds only
    // while the write stays on this one well-understood path: a second
    // assignment site, or one hoisted out of refreshTreeOrdering into a
    // handler that fires during a reset, re-opens a use-after-free that
    // release builds pass by luck.
    //
    // This guard was added when refreshTreeOrdering()'s ~150 lines of
    // derivation moved to TreeOrdering::compute() (2026-07-20). That refactor
    // deliberately changed only WHERE the ordering is computed, never WHEN or
    // HOW it is assigned — this pins the half that must not move.
    void sidebar_assignsFileSortKeysExactlyOnce()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        // Assignments only — `searchProxy.fileSortKeys =`, not a read.
        static const QRegularExpression assign(
            QStringLiteral("searchProxy\\s*\\.\\s*fileSortKeys\\s*=[^=]"));
        int writes = 0;
        auto it = assign.globalMatch(src);
        while (it.hasNext()) {
            it.next();
            ++writes;
        }
        QCOMPARE(writes, 1);

        const QString body = functionBody(src, QStringLiteral("function refreshTreeOrdering()"));
        QVERIFY2(!body.isEmpty(), "refreshTreeOrdering must exist");
        QVERIFY2(body.contains(QStringLiteral("searchProxy.fileSortKeys =")),
                 "the one fileSortKeys assignment must live in refreshTreeOrdering() — "
                 "moving it to a handler that can fire mid source-model reset re-opens "
                 "the deferred-invalidate use-after-free");
    }

    // Regression (queue 070): an in-place file rename updates the tree via
    // VaultTreeModel::applyRenameOutcome WITHOUT a modelReset, so onModelReset
    // never re-runs refreshTreeOrdering. fileSortKeys is keyed by path, so the
    // renamed file's new path would have no key and sink into the proxy's
    // keyless tier (VaultSearchProxyModel::lessThan) — the row slides down
    // under an mtime sort though its mtime never changed. onFileRenamed must
    // recompute the ordering itself. Source-scan: the real no-reset rename
    // path isn't reachable from tst_sidebar_rename's stub controller.
    void sidebar_reordersTreeOnInPlaceFileRename()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");
        const QString body = functionBody(src, QStringLiteral("function onFileRenamed("));
        QVERIFY2(!body.isEmpty(), "onFileRenamed must exist");
        QVERIFY2(body.contains(QStringLiteral("refreshTreeOrdering")),
                 "onFileRenamed must recompute fileSortKeys after an in-place "
                 "rename — else the renamed path has no sort key and its row "
                 "drops below every keyed audio file in the folder");
    }

    // Regression (queue 001): a freshly-formed combine group reads as "pending"
    // (newest member inside the stability window) and is absent from
    // combineReadyPaths, so it shows no candidate outline. "pending" resolves
    // only by the passage of time, and nothing else re-runs the scan once the
    // file settles — so refreshTreeOrdering() must re-schedule itself while any
    // group is pending, else the outline lags one filesystem event behind.
    void sidebar_reschedulesCombineScanWhilePending()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");
        const QString body = functionBody(src, QStringLiteral("function refreshTreeOrdering()"));
        QVERIFY2(!body.isEmpty(), "refreshTreeOrdering must exist");
        QVERIFY2(body.contains(QStringLiteral("pending")),
                 "refreshTreeOrdering must detect a pending combine group");
        QVERIFY2(body.contains(QStringLiteral("combinePendingRescanTimer")),
                 "refreshTreeOrdering must re-arm a re-scan while a group is "
                 "pending, else the combine outline lags one event behind");
        // The re-scan timer itself must exist and re-run the ordering.
        QVERIFY2(src.contains(QStringLiteral("id: combinePendingRescanTimer")),
                 "combinePendingRescanTimer must be declared");
    }

    // The gate that drops task-related snackbars (Notifications.tasksPopupOpen)
    // is dead code unless something sets it. BottomBar must Bind it to the Tasks
    // popup's visibility. Stripped first — the change adds doc comments that name
    // both tokens, which a raw scan would match without the wiring existing.
    void bottomBar_bindsTasksPopupOpen()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/BottomBar.qml must exist");
        QVERIFY2(src.contains(QStringLiteral("tasksPopupOpen")),
                 "BottomBar must publish the Tasks popup's open state to Notifications");
        QVERIFY2(src.contains(QStringLiteral("jobListPopup.visible")),
                 "the tasksPopupOpen Binding must track the Tasks popup's visibility");
    }

    // The Tasks popup's per-row kind badge ("Detect speakers", "Generate
    // chapters", …) must WRAP rather than only elide on one line, and the row
    // must grow to fit the wrapped badge — otherwise the longest kinds get
    // truncated. Source-scan (QML delegate inside a Popup, no QQuickView).
    void tasksPopup_kindBadgeWrapsAndRowGrows()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/BottomBar.qml must exist");
        const QString block = enclosingBlock(src, QStringLiteral("id: kindLabel"));
        QVERIFY2(!block.isEmpty(), "the Tasks popup per-row kind badge must have id: kindLabel");
        QVERIFY2(block.contains(QStringLiteral("wrapMode: Text.WordWrap")),
                 "the kind badge must wrap (Text.WordWrap), not just elide, so long "
                 "kind labels like \"Generate chapters\" aren't truncated on one line");
        QVERIFY2(block.contains(QStringLiteral("maximumLineCount: 2")),
                 "the wrapped kind badge must be capped at 2 lines so a pathological "
                 "label can't balloon the row height");
        QVERIFY2(src.contains(QStringLiteral("Math.max(24, kindLabel.implicitHeight")),
                 "the task row height must grow with the wrapped kind badge "
                 "(floored at 24) — a fixed height:24 would clip the second line");
    }

    // The VAD status dot beside the transcription model dropdown: green
    // (Theme.success) when the selected model runs through the app's Silero VAD
    // path, red (Theme.danger) when the engine self-segments and bypasses it,
    // keyed on the controller's modelUsesVad() (NOT a QML-side copy of the
    // engine-id convention — that's a parity seam). It ALSO flips red on a
    // runtime failure: the open file's stored transcript recorded a VAD failure
    // (metadata.vadError), the same signal the fallback banner reads. Carries a
    // hover tooltip explaining VAD. Source-scan (QML element in MainPanel, no
    // QQuickView here).
    void mainPanel_vadIndicatorReflectsModelUsesVad()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");
        const QString block = enclosingBlock(src, QStringLiteral("id: vadIndicator"));
        QVERIFY2(!block.isEmpty(), "the VAD status dot must have id: vadIndicator");
        QVERIFY2(block.contains(QStringLiteral("modelUsesVad")),
                 "the VAD dot's state must come from the controller's modelUsesVad(), "
                 "not a QML-side duplicate of the engine-id convention");
        QVERIFY2(block.contains(QStringLiteral("metadata.vadError")),
                 "the VAD dot must also flip red on a runtime VAD failure recorded "
                 "in the open file's metadata (metadata.vadError)");
        QVERIFY2(block.contains(QStringLiteral("modelUsesVadGating")),
                 "the green tooltip must scope its silence-skipping claim to whisper "
                 "via modelUsesVadGating() — parakeet gets windowing but not gating");
        QVERIFY2(block.contains(QStringLiteral("Theme.success")) && block.contains(QStringLiteral("Theme.danger")),
                 "the VAD dot must be green (Theme.success) when VAD is used and red "
                 "(Theme.danger) when it is not");
        QVERIFY2(block.contains(QStringLiteral("ToolTip")),
                 "the VAD dot must carry a hover ToolTip explaining what VAD does");
    }

    // 005: the onFinished live-reload gate must compare the finished file to the
    // open file by LOCAL PATH (controller.isSameFile), not
    // fileUrl.toString() === player.source.toString(). A detect-speakers run on a
    // spaced filename emits a fileUrl that can stringify differently from
    // player.source (literal ' ' vs "%20"), so the old toString() compare
    // silently skipped the reload and the new speaker tags never appeared live.
    // Source-scan (QML element in MainPanel, no QQuickView here).
    void mainPanel_onFinishedReloadComparesByLocalPath()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");
        QVERIFY2(src.contains(QStringLiteral("isSameFile(fileUrl, mainPanel.player.source)")),
                 "onFinished's live-reload gate must use controller.isSameFile(fileUrl, player.source) "
                 "(local-path compare) so a detect-speakers run on a spaced filename still refreshes");
        QVERIFY2(!src.contains(QStringLiteral("fileUrl.toString() === mainPanel.player.source.toString()")),
                 "the fragile toString()-based onFinished reload compare must be gone — it silently "
                 "skips the live reload when a spaced filename stringifies differently on each side");
    }

    // A download triggered by transcribing an undownloaded model fires the
    // controller's central modelsChanged() (DownloadTask::completionChangesModelSet).
    // MainPanel's three model pickers must therefore NOT bind model: to the
    // untracked availableModels()/availableSegmentationModels()/
    // availableEmbeddingModels() invokables (they evaluate once, so a picker's
    // downloaded dot never flips green live) — they read imperatively-refreshed
    // properties re-fetched on modelsChanged(), the ModelsPage/BenchmarkPage fix.
    void mainPanel_modelPickersRefreshOnModelsChanged()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");
        QVERIFY2(!src.contains(QStringLiteral(
                     "model: mainPanel.transcriptionController ? mainPanel.transcriptionController.availableModels()")),
                 "MainPanel's transcription picker must not bind model: to the untracked "
                 "availableModels() invokable (it evaluates once) — bind an imperatively "
                 "refreshed property so a transcribe-triggered download flips the dot on modelsChanged()");
        QVERIFY2(!src.contains(QStringLiteral(
                     "model: mainPanel.transcriptionController ? mainPanel.transcriptionController.availableSegmentationModels()")),
                 "MainPanel's segmentation picker must read a refreshed property, not the untracked invokable");
        QVERIFY2(!src.contains(QStringLiteral(
                     "model: mainPanel.transcriptionController ? mainPanel.transcriptionController.availableEmbeddingModels()")),
                 "MainPanel's embedding picker must read a refreshed property, not the untracked invokable");
        QVERIFY2(src.contains(QStringLiteral("onModelsChanged")),
                 "MainPanel must refresh its model pickers on the controller's modelsChanged()");
    }

    // The search-result blurb (transcriptSnippet) must make the matched
    // substring visibly stand out on the muted grey snippet: bold + a foreground
    // color, not the dark-blue background-color alone (which barely reads).
    // Queue item 017.
    void searchBlurbMatchIsBoldAndRecolored()
    {
        // transcriptSnippet() moved out of Sidebar.qml into the shared pure
        // helper module qml/TextFormat.js (2026-07-20); the guard follows the
        // code, its intent unchanged.
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/TextFormat.js")));
        QVERIFY2(!src.isEmpty(), "qml/TextFormat.js must exist");
        const QString snip = functionBody(src, QStringLiteral("function transcriptSnippet"));
        QVERIFY2(!snip.isEmpty(), "transcriptSnippet() must exist in TextFormat.js");
        QVERIFY2(snip.contains(QStringLiteral("font-weight:bold")),
                 "the blurb match span must be bold");
        QVERIFY2(snip.contains(QStringLiteral("color:white")),
                 "the blurb match span must set a foreground color, not background-color alone");
    }

    // The undo snackbar is the post the whole suppression feature is about; a
    // future rewrite of onUndoAvailable must not silently drop the taskRelated
    // tag. functionBody so a taskRelated: true anywhere else in Main.qml can't
    // satisfy this by accident.
    void undoSnackbar_isTaskRelated()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml")));
        const QString body = functionBody(src, QStringLiteral("function onUndoAvailable"));
        QVERIFY2(!body.isEmpty(), "Main.qml must have an onUndoAvailable handler");
        QVERIFY2(body.contains(QStringLiteral("taskRelated: true")),
                 "the generic undo snackbar must be tagged taskRelated so it drops "
                 "while the Tasks popup is open");
    }

    // Regression: Sidebar.qml's treeDelegate (a TreeView row) recycles a
    // fixed pool of delegate Items as rows scroll in/out of view. Required
    // properties bound to model roles (path/isAudio/label/...) get reassigned
    // for the recycled Item's new row automatically, but jobStatus is a plain
    // imperative property (jobStatusForPath() is a Q_INVOKABLE, not a role),
    // pushed at Component.onCompleted. Without an onPathChanged refresh, a
    // recycled Item kept showing whichever job-status it FIRST loaded, forever.
    // (The label used to have the same hazard via an imperative `metadata`
    // property — that's now the LabelRole required property, so recycling
    // handles it and only jobStatus still needs the manual refresh.) No
    // QQuickView here to drive TreeView recycling, so this is a source-scan
    // guard against the fix regressing rather than a behavioral test.
    void treeDelegate_refreshesJobStatusOnRecycle()
    {
        const QString src = sidebarTreeSrc();
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");
        // The label side of the recycling fix: it must be a bound role, not an
        // imperative push, so it can't go stale on reuse.
        QVERIFY2(src.contains(QStringLiteral("required property string label")),
                 "treeDelegate must bind the label model role (not an imperative metadata push), "
                 "so a recycled row shows the right label automatically");
        // The jobStatus side still needs the manual onPathChanged refresh.
        const int idx = src.indexOf(QStringLiteral("onPathChanged: {"));
        QVERIFY2(idx >= 0,
                 "treeDelegate must refresh jobStatus onPathChanged — TreeView recycles delegate "
                 "Items across rows, so a Component.onCompleted-only push goes stale on reuse");
        const QString after = src.mid(idx, 300);
        QVERIFY2(after.contains(QStringLiteral("refreshJobStatus()")),
                 "onPathChanged must call refreshJobStatus() so recycled rows show the right job status");
    }

    // Regression: treeDelegate's filename Text had no width bound at all, so
    // a long filename (routine for split-recording combine candidates — the
    // very files Item 51's groupMergeButton targets) painted straight
    // through the per-row groupMergeButton overlay on the right instead of
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
        const QString src = sidebarTreeSrc();
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");
        const int rowIdx = src.indexOf(QStringLiteral("id: groupMergeButton"));
        QVERIFY2(rowIdx >= 0, "groupMergeButton must exist in Sidebar.qml's treeDelegate");
        const QString before = src.left(rowIdx);
        const int textIdx = before.indexOf(QStringLiteral("useLabelsSidebar"));
        QVERIFY2(textIdx >= 0, "treeDelegate's filename Text (AppSettings.useLabelsSidebar) must exist "
                               "before the per-row groupMergeButton overlay");
        const QString filenameTextBlock = before.mid(textIdx, before.length() - textIdx);
        QVERIFY2(filenameTextBlock.contains(QStringLiteral("Text.ElideRight"))
                     && filenameTextBlock.contains(QStringLiteral("Text.ElideNone")),
                 "treeDelegate's filename Text must elide when plain text (otherwise a long filename "
                 "overlaps the per-row groupMergeButton) but must be able to go elide-less too (needed "
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
        // The width/color default to the 1px accent hover border, but are now
        // overridable props (a stronger "active page" border reuses the same
        // component — see TitleBar's page buttons), so the guard checks the
        // parametrized form plus its 1px/accent defaults rather than literals.
        QVERIFY(src.contains(QStringLiteral("border.width: borderWidth")));
        QVERIFY(src.contains(QStringLiteral("property int borderWidth: 1")));
        QVERIFY(src.contains(QStringLiteral("border.color: borderColor")));
        QVERIFY2(src.contains(QStringLiteral("property color borderColor: AppSettings.accentColor")),
                 "hover outline color must default to the configurable AppSettings.accentColor, not a "
                 "hardcoded \"white\"");
        QVERIFY2(src.contains(QStringLiteral("Behavior on opacity")),
                 "hover outline must fade, not pop (StyleGuide.md)");
    }

    // Item: the full-panel blocking overlay must name the ACTUAL task (keyed
    // off currentPhase), not a hardcoded "Transcribing…". The phase→title
    // mapping now lives in the controller's phase registry
    // (phaseOverlayLabel), not a switch here — see phaseLabelSwitchesStayRetired
    // for the no-switch guard and phaseRegistryCoversEveryEmittedPhase for
    // coverage.
    void mainPanel_blockingOverlayNamesTheTask()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(src.contains(QStringLiteral("property string transcribingOverlayLabel")),
                 "the overlay label must be derived (transcribingOverlayLabel), not fixed");
        QVERIFY2(src.contains(QStringLiteral("text: mainPanel.transcribingOverlayLabel")),
                 "the overlay's Label must bind to the derived task label");
        QVERIFY2(src.contains(QStringLiteral("phaseOverlayLabel(")),
                 "the overlay label must derive its title from the controller's phase registry "
                 "(phaseOverlayLabel), not a QML phase switch");
    }

    // The per-file "regenerate auto-title" button runs relabel(), which
    // regenerates label, prelude AND tags (LabelingEngine's {label, prelude,
    // tags} payload) — the tooltip must name all three, not just label +
    // prelude (its old wording undersold it by omitting tags). Queue item 020.
    void mainPanel_regenerateTooltipNamesTags()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");
        QVERIFY2(src.contains(QStringLiteral("Regenerate label, prelude, and tags")),
                 "the regenerate-label tooltip must name tags, since relabel() regenerates them");
        QVERIFY2(!src.contains(QStringLiteral("Regenerate label and prelude")),
                 "the old tooltip wording (no tags) must be gone");
    }

    // Item: timestamps show on EVERY sentence line; speaker names stay one per
    // turn. The two gutter models are what encode that rule.
    void mainPanel_timestampAlways_speakerOnTurn()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        // Silence phase 3 (queue item 085) added a second pseudo-entry kind
        // (isSilenceMarker) that must ALSO be excluded, same as a chapter
        // heading — see mainPanel_silenceMarkerMatchesStyleGuide for the
        // dedicated guard on that exclusion.
        QVERIFY2(src.contains(QStringLiteral("displaySentences.filter(s => !s.isChapterHeading && !s.isSilenceMarker)")),
                 "the timestamp gutter must show one timestamp per (non-heading, non-silence-marker) "
                 "sentence, always");
        QVERIFY2(src.contains(QStringLiteral("s.showGutter && s.speaker !== \"\"")),
                 "the speaker gutter must stay one-per-turn (showGutter + non-empty speaker)");
    }

    // Both per-device USB grabber toggles (enabled + auto-grab, on the
    // catch-all card and each device card) must be labeled pills, not bare
    // Switches — the enabled one used to have no text at all, so it was
    // indistinguishable from the auto-grab switch beside it. They now use the
    // reusable UsbToggle (modeled on Sidebar's search "Transcriptions" toggle),
    // each carrying a visible "Enabled"/"Auto-grab" label.
    void usbGrabberToggles_areLabeledPills()
    {
        // The USB grabber screen was extracted out of MainPanel into its own
        // UsbGrabberPage.qml component (2026-07-19 decomposition), so its
        // toggles live there now, not in MainPanel.qml.
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/UsbGrabberPage.qml")));
        QVERIFY2(!src.isEmpty(), "qml/UsbGrabberPage.qml must exist");
        QVERIFY2(src.contains(QStringLiteral("UsbToggle {")),
                 "the USB grabber toggles must be the labeled UsbToggle pill, not a bare Switch");
        QVERIFY2(src.contains(QStringLiteral("text: qsTr(\"Enabled\")")),
                 "the enabled toggle must carry a visible \"Enabled\" label");
        QVERIFY2(src.contains(QStringLiteral("text: qsTr(\"Auto-grab\")")),
                 "the auto-grab toggle must carry a visible \"Auto-grab\" label");
        // The old bare, unlabeled enable switches must be gone (their ambiguity
        // is exactly what this change fixes).
        QVERIFY2(!src.contains(QStringLiteral("id: deviceEnabledSwitch")),
                 "the bare deviceEnabledSwitch must be replaced by a labeled UsbToggle");
        QVERIFY2(!src.contains(QStringLiteral("id: catchAllEnabledSwitch")),
                 "the bare catchAllEnabledSwitch must be replaced by a labeled UsbToggle");
    }

    // Queue item 068: the USB grabber top-level screen was pulled out of the
    // 6600-line MainPanel.qml into its own UsbGrabberPage.qml component. This
    // is the sanctioned source-scan guard (no QQuickView harness for the page)
    // that the extraction stays done — MainPanel must instantiate the component
    // rather than re-inlining its ~800-line body.
    void usbGrabberPage_extractedToOwnFile()
    {
        const QString page = readAll(QStringLiteral(SUOP_QML_DIR "/UsbGrabberPage.qml"));
        QVERIFY2(!page.isEmpty(), "qml/UsbGrabberPage.qml must exist");
        QVERIFY2(page.contains(QStringLiteral("id: usbGrabberPage")),
                 "the extracted component's root must keep id: usbGrabberPage");
        QVERIFY2(page.contains(QStringLiteral("property UsbGrabberController usbGrabberController")),
                 "the component must expose usbGrabberController as a wired-in property, "
                 "not reach into a host mainPanel");

        const QString panel = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!panel.isEmpty(), "qml/MainPanel.qml must exist");
        QVERIFY2(panel.contains(QStringLiteral("UsbGrabberPage {")),
                 "MainPanel must instantiate the extracted UsbGrabberPage component");
        QVERIFY2(!panel.contains(QStringLiteral("function refreshDevices()")),
                 "MainPanel must no longer inline the USB page body (refreshDevices() "
                 "moved into UsbGrabberPage.qml)");
        QVERIFY2(!panel.contains(QStringLiteral("property var mountedDevices")),
                 "MainPanel must no longer inline the USB page's mountedDevices snapshot "
                 "(it moved into UsbGrabberPage.qml)");
    }

    // Queue item 069: the recorder top-level screen (and its recordingFolderDialog
    // FolderDialog) was pulled out of MainPanel.qml into its own RecorderPage.qml
    // component. Sanctioned source-scan guard (no QQuickView harness for the page)
    // that the extraction stays done — MainPanel must instantiate the component
    // rather than re-inlining its ~500-line body.
    void recorderPage_extractedToOwnFile()
    {
        const QString page = readAll(QStringLiteral(SUOP_QML_DIR "/RecorderPage.qml"));
        QVERIFY2(!page.isEmpty(), "qml/RecorderPage.qml must exist");
        QVERIFY2(page.contains(QStringLiteral("id: recorderPage")),
                 "the extracted component's root must keep id: recorderPage");
        QVERIFY2(page.contains(QStringLiteral("property AudioRecorder audioRecorder")),
                 "the component must expose audioRecorder as a wired-in property, "
                 "not reach into a host mainPanel");
        QVERIFY2(page.contains(QStringLiteral("signal openVaultRequested()")),
                 "the component must re-expose openVaultRequested as its own signal "
                 "for the host to forward");

        const QString panel = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!panel.isEmpty(), "qml/MainPanel.qml must exist");
        QVERIFY2(panel.contains(QStringLiteral("RecorderPage {")),
                 "MainPanel must instantiate the extracted RecorderPage component");
        QVERIFY2(!panel.contains(QStringLiteral("function refreshMicDevices()")),
                 "MainPanel must no longer inline the recorder page body "
                 "(refreshMicDevices() moved into RecorderPage.qml)");
        QVERIFY2(!panel.contains(QStringLiteral("id: waveformCanvas")),
                 "MainPanel must no longer inline the recorder waveform Canvas "
                 "(it moved into RecorderPage.qml)");
        QVERIFY2(!panel.contains(QStringLiteral("id: recordingFolderDialog")),
                 "MainPanel must no longer inline the recorder folder picker "
                 "(it moved into RecorderPage.qml)");
    }

    // Item: the filename subtitle is an editable rename control for the real
    // file, and chapter collapse persists to frontmatter.
    void mainPanel_editableFilename_andPersistedCollapse()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(src.contains(QStringLiteral("function commitFilenameRename(")),
                 "the filename subtitle must commit a real file rename");
        QVERIFY2(src.contains(QStringLiteral("function initCollapsedFromFrontmatter("))
                     && src.contains(QStringLiteral("function persistCollapsedChapters(")),
                 "chapter collapse must round-trip through frontmatter (persist + restore)");
        QVERIFY2(src.contains(QStringLiteral("setCollapsedChapters(")),
                 "collapse must persist via the controller's setCollapsedChapters invokable");
    }

    // The playback controls must be inert when there's no audio loaded — the
    // seek slider was draggable with nothing open (its track spans the panel
    // whenever the orphaned-md view isn't showing, including the nothing-open
    // state). Both the play button and the seek slider gate their `enabled` on
    // the SAME hasPlayableSource binding so they can never disagree — and being
    // a plain binding on player.source, it also greys out the instant a
    // deletion closes the current file (onFileDeleted sets player.source = "").
    void mainPanel_playbackControlsInertWithNoAudio()
    {
        const QString src = stripComments(mainPanelSrc());
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("readonly property bool hasPlayableSource")),
                 "MainPanel must expose a single hasPlayableSource binding both playback controls share");

        const QString slider = enclosingBlock(src, QStringLiteral("id: seekSlider"));
        QVERIFY2(!slider.isEmpty(), "id: seekSlider must exist in MainPanel.qml");
        QVERIFY2(slider.contains(QStringLiteral("enabled: mainPanel.hasPlayableSource")),
                 "the seek slider must be disabled when no audio is loaded, or it can be dragged with "
                 "nothing open");

        const QString playButton = enclosingBlock(src, QStringLiteral("id: playPauseButton"));
        QVERIFY2(!playButton.isEmpty(), "id: playPauseButton must exist in MainPanel.qml");
        QVERIFY2(playButton.contains(QStringLiteral("enabled: mainPanel.hasPlayableSource")),
                 "the play/pause button must gate on the same hasPlayableSource binding as the slider");
    }

    // Item: the sentence highlight only makes sense while audio is playing, and
    // while it IS playing something must always be lit.
    //   1. Orphaned .md (no audio) → the binding must bail to -1 up front, or a
    //      leftover player.position of 0 sticks the highlight on any transcript
    //      whose first segment is at t0=0 (i.e. almost all of them).
    //   2. With audio, the C++ search returns -1 before the first t0; the
    //      binding must clamp so one sentence is always highlighted.
    void mainPanel_activeSentenceGuardedByAudioAndClamped()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const QString binding =
            functionBody(src, QStringLiteral("readonly property int activeSentenceIndex"));
        QVERIFY2(!binding.isEmpty(), "activeSentenceIndex binding must exist in MainPanel.qml");

        QVERIFY2(binding.contains(QStringLiteral("hasAudio")),
                 "activeSentenceIndex must gate on hasAudio so an orphaned .md (no audio) highlights "
                 "nothing instead of sticking on the top sentence");
        // Queue item 010: the front-edge clamp used to be Math.max(0, idx), which
        // lit displaySentences[0] — a zero-length chapter-heading pseudo-entry
        // when a chapter opens the transcript, painting a stray highlight sliver
        // over its title. It now clamps to the first REAL (spoken) sentence.
        QVERIFY2(binding.contains(QStringLiteral("firstRealSentenceIndex")),
                 "activeSentenceIndex must clamp the C++ result up to the first REAL sentence "
                 "(firstRealSentenceIndex) so one spoken sentence stays highlighted before playback "
                 "reaches its t0 — never index 0, which may be a chapter heading");
        QVERIFY2(!binding.contains(QStringLiteral("Math.max(0")),
                 "the old Math.max(0, idx) clamp must be gone — it highlighted a leading chapter heading");
    }

    // Queue item 054: when the playback highlight rests on a collapsed chapter's
    // heading (its only visible line), activeSentenceRect must paint the WHOLE
    // heading row (gutterLineRects, full width), not the zero-width sliver that
    // the heading's start/length would give over its blank reserved line. No
    // NULL-controller QML harness can exercise the on-screen highlight, so guard
    // the source.
    void mainPanel_activeSentenceRectPaintsCollapsedHeadingRow()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        const QString body = functionBody(src, QStringLiteral("readonly property rect activeSentenceRect"));
        QVERIFY2(!body.isEmpty(), "activeSentenceRect binding must exist in MainPanel.qml");
        QVERIFY2(body.contains(QStringLiteral("isChapterHeading"))
                     && body.contains(QStringLiteral("gutterLineRects")),
                 "activeSentenceRect must paint the full heading row (gutterLineRects) for a "
                 "collapsed chapter's heading, not a zero-width sliver (item 054)");
    }

    // Item: while audio plays the transcript follows the playhead, pinning the
    // active sentence near the top; a toggle beside the play button disables it,
    // defaulting to on (AppSettings.followPlaybackScroll: true).
    void mainPanel_autoScrollFollowsPlayback()
    {
        const QString src = stripComments(mainPanelSrc());
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        // The scroll routine must gate on BOTH the toggle and active playback —
        // otherwise it yanks the viewport while paused, or ignores the setting.
        const QString scroll =
            functionBody(src, QStringLiteral("function scrollActiveSentenceToTop()"));
        QVERIFY2(!scroll.isEmpty(), "scrollActiveSentenceToTop() must exist in MainPanel.qml");
        QVERIFY2(scroll.contains(QStringLiteral("AppSettings.followPlaybackScroll")),
                 "auto-scroll must honor the followPlaybackScroll toggle");
        QVERIFY2(scroll.contains(QStringLiteral(".playing")),
                 "auto-scroll must only chase the playhead while audio is playing, "
                 "so a paused user can scroll freely");

        // It must actually be driven — re-pinned as the active sentence advances.
        QVERIFY2(src.contains(QStringLiteral("onActiveSentenceIndexChanged: mainPanel.scrollActiveSentenceToTop()")),
                 "the active sentence advancing must re-run scrollActiveSentenceToTop");

        // The toggle button beside the play button, with its AppIcon.
        const QString button = enclosingBlock(src, QStringLiteral("id: followPlaybackButton"));
        QVERIFY2(!button.isEmpty(), "the follow-playback toggle button must exist in MainPanel.qml");
        QVERIFY2(button.contains(QStringLiteral("AppSettings.followPlaybackScroll = !AppSettings.followPlaybackScroll")),
                 "the toggle must flip the followPlaybackScroll setting");
        QVERIFY2(button.contains(QStringLiteral("name: \"follow-playback\"")),
                 "the toggle must draw the follow-playback AppIcon");

        // Default on: scrolling with the audio is the out-of-box behavior.
        const QString settings = readAll(QStringLiteral(SUOP_QML_DIR "/AppSettings.qml"));
        QVERIFY2(settings.contains(QStringLiteral("property bool followPlaybackScroll: true")),
                 "followPlaybackScroll must default to true (scroll with audio by default)");
    }

    // Silence phase 2 (queue item 084): while playing, the head auto-skips
    // past a stored `silences:` span (phase 1) whose length is >= the
    // configured threshold — but ONLY when the toggle is on. A bare
    // player.positionChanged handler with no gate would rewrite
    // player.position (a real seek side effect) on every single tick.
    // Source-scan since there's no QQuickView harness for a live player's
    // positionChanged binding (CLAUDE.md "no direct unit-test path").
    void mainPanel_skipSilenceGatedOnToggle()
    {
        const QString src = stripComments(mainPanelSrc());
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const QString handler = functionBody(src, QStringLiteral("function onPositionChanged()"));
        QVERIFY2(!handler.isEmpty(), "the skip-silence player.positionChanged handler must exist");
        QVERIFY2(handler.contains(QStringLiteral("AppSettings.skipSilenceEnabled")),
                 "the skip-silence seek must be gated on AppSettings.skipSilenceEnabled");
        QVERIFY2(handler.contains(QStringLiteral("nextSilenceSkipTarget")),
                 "the skip-silence seek must consume stored spans via nextSilenceSkipTarget — "
                 "changing the threshold must never re-analyze audio (spans are stored raw in phase 1)");
        QVERIFY2(handler.contains(QStringLiteral(".playing")),
                 "the skip-silence seek must only fire while audio is actually playing");

        // The toggle button beside followPlaybackButton, with its own AppIcon.
        const QString button = enclosingBlock(src, QStringLiteral("id: skipSilenceButton"));
        QVERIFY2(!button.isEmpty(), "the skip-silence toggle button must exist in MainPanel.qml");
        QVERIFY2(button.contains(QStringLiteral("AppSettings.skipSilenceEnabled = !AppSettings.skipSilenceEnabled")),
                 "the toggle must flip the skipSilenceEnabled setting");
        QVERIFY2(button.contains(QStringLiteral("name: \"skip-silence\"")),
                 "the toggle must draw the skip-silence AppIcon");

        // Default off (a playback-behavior change must not silently switch on
        // for everyone) at 3s (locked Decision, queue item 084).
        const QString settings = readAll(QStringLiteral(SUOP_QML_DIR "/AppSettings.qml"));
        QVERIFY2(settings.contains(QStringLiteral("property bool skipSilenceEnabled: false")),
                 "skipSilenceEnabled must default to false");
        QVERIFY2(settings.contains(QStringLiteral("property real skipSilenceThresholdSeconds: 3")),
                 "skipSilenceThresholdSeconds must default to 3 seconds");
    }

    // Silence phase 3 (queue item 085): the visual between-segment divider,
    // per StyleGuide.md "Transcript silence divider" — a fixed glyph form
    // ("··· <duration> silence ···", U+00B7 MIDDLE DOT), gated on its OWN
    // fixed minimum-duration constant (never AppSettings.skipSilenceThreshold
    // Seconds — that one is phase 2's user-configurable playback-skip
    // threshold, an unrelated concern per the StyleGuide entry), and never
    // wired into the sentence-level highlight binsearch as landable content.
    // Source-scan since there's no QQuickView harness for the
    // displaySentences/gutterLineRects overlay wiring this depends on.
    void mainPanel_silenceMarkerMatchesStyleGuide()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        // The glyph form lives in exactly one function — which moved out of
        // MainPanel.qml into qml/TextFormat.js (2026-07-20). MainPanel keeps a
        // one-line wrapper; the glyphs themselves are guarded where they live.
        const QString textFormatSrc =
            stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/TextFormat.js")));
        QVERIFY2(!textFormatSrc.isEmpty(), "qml/TextFormat.js must exist");
        const QString formatter =
            functionBody(textFormatSrc, QStringLiteral("function formatSilenceMarkerText("));
        QVERIFY2(!formatter.isEmpty(), "formatSilenceMarkerText must exist in TextFormat.js");
        QVERIFY2(formatter.contains(QStringLiteral("··· %1 silence ···")),
                 "the divider text must read \"··· <duration> silence ···\" "
                 "(StyleGuide.md \"Transcript silence divider\")");

        // A fixed, independent minimum-duration gate for SHOWING a marker —
        // never AppSettings.skipSilenceThresholdSeconds (that's the phase 2
        // playback-skip threshold, a different, user-configurable concern).
        QVERIFY2(src.contains(QStringLiteral("readonly property real silenceMarkerMinSeconds: 5")),
                 "silenceMarkerMinSeconds must exist and default to 5 (StyleGuide.md)");
        // displayData is no longer a braced JS block — the assembly moved to
        // src/core/TranscriptDisplayBuilder.cpp (2026-07-20) and the binding
        // is now a single call into it — so it's bounded by bindingCall()
        // (paren-balanced) rather than functionBody(), which needs a brace.
        const QString displayData =
            bindingCall(src, QStringLiteral("readonly property var displayData:"));
        QVERIFY2(!displayData.isEmpty(), "the displayData binding must exist in MainPanel.qml");
        QVERIFY2(displayData.contains(QStringLiteral("TranscriptDisplayBuilder.build(")),
                 "displayData must be the C++ display-model builder's output");
        QVERIFY2(displayData.contains(QStringLiteral("mainPanel.silenceMarkerMinSeconds")),
                 "a stored silence span must be gated on silenceMarkerMinSeconds before it earns a "
                 "divider, or every ordinary inter-sentence gap would show one");
        QVERIFY2(!displayData.contains(QStringLiteral("skipSilenceThresholdSeconds")),
                 "the visual marker's threshold must stay independent of the phase 2 playback-skip "
                 "threshold — conflating them would make the skip slider silently reshape the marking too");
        // ...and the builder must actually USE it as the gate, rather than
        // accepting the argument and ignoring it.
        const QString builder = stripComments(
            readAll(QStringLiteral(SUOP_SRC_DIR "/core/TranscriptDisplayBuilder.cpp")));
        QVERIFY2(!builder.isEmpty(), "src/core/TranscriptDisplayBuilder.cpp must exist");
        QVERIFY2(builder.contains(QStringLiteral(">= minSilenceSeconds")),
                 "the builder must gate a span on the passed-in minimum before pushing a divider");

        // Every pseudo-entry pushSilenceMarker reserves is flagged so it
        // never becomes real spoken content: excluded from the timestamp
        // gutter, from firstRealSentenceIndex, and from what an edit commits.
        // gutterTimestampModel is a one-line filter expression, not a
        // brace-delimited function — functionBody()/enclosingBlock() need a
        // brace to match, which this binding doesn't have — so scan its
        // statement directly instead (bounded to the two lines starting at
        // the anchor, matching how this binding is actually formatted).
        const int gutterIdx = src.indexOf(QStringLiteral("readonly property var gutterTimestampModel:"));
        QVERIFY2(gutterIdx >= 0, "gutterTimestampModel must exist in MainPanel.qml");
        const int gutterStatementEnd = src.indexOf(QLatin1Char('\n'), src.indexOf(QLatin1Char('\n'), gutterIdx) + 1);
        const QString gutterStatement = src.mid(gutterIdx, gutterStatementEnd - gutterIdx);
        QVERIFY2(gutterStatement.contains(QStringLiteral("!s.isSilenceMarker")),
                 "gutterTimestampModel must exclude silence-marker pseudo-entries, or the divider line "
                 "gets a stray timestamp label");

        const QString commit = functionBody(src, QStringLiteral("function commitTranscriptEdits("));
        QVERIFY2(!commit.isEmpty(), "commitTranscriptEdits must exist in MainPanel.qml");
        QVERIFY2(commit.contains(QStringLiteral("entry.isChapterHeading || entry.isSilenceMarker")),
                 "commitTranscriptEdits must skip silence-marker reserved lines the same way it skips "
                 "chapter headings, or editing a transcript with dividers corrupts on save");

        // The overlay Repeater itself: full-width, centered, never
        // interactive (no HoverHandler/TapHandler/HoverBorder — it's pure
        // decoration, not a control).
        const QString repeater = enclosingBlock(src, QStringLiteral("id: silenceMarkerRepeater"));
        QVERIFY2(!repeater.isEmpty(), "the silenceMarkerRepeater overlay must exist in MainPanel.qml");
        QVERIFY2(repeater.contains(QStringLiteral("mainPanel.silenceMarkerModel")),
                 "silenceMarkerRepeater must be driven by silenceMarkerModel");
        QVERIFY2(repeater.contains(QStringLiteral("Text.AlignHCenter")),
                 "the divider text must be horizontally centered");
        QVERIFY2(repeater.contains(QStringLiteral("width: transcriptArea.width")),
                 "the divider row must span the full transcript width, gutter columns included");
        QVERIFY2(repeater.contains(QStringLiteral("color: \"#5a5a5a\"")),
                 "the divider color must match StyleGuide.md's #5a5a5a (dimmer than the #777777 "
                 "timestamp gutter)");
        QVERIFY2(repeater.contains(QStringLiteral("font.italic: true")) && repeater.contains(QStringLiteral("font.pixelSize: 11")),
                 "the divider typography must match StyleGuide.md (italic, 11px)");
        QVERIFY2(!repeater.contains(QStringLiteral("TapHandler")) && !repeater.contains(QStringLiteral("HoverHandler")),
                 "the silence divider must stay non-interactive — no hover/tap handling");
    }

    // Item: a dedicated drag-handle grip carries the speaker merge gesture (so a
    // plain click can still edit the name) — its own AppIcon, not overloading
    // the pill body (StyleGuide "Always-editable text").
    void mainPanel_speakerDragHandleGrip()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(src.contains(QStringLiteral("id: speakerDragHandle")),
                 "the speaker pill must have a dedicated drag grip");
        QVERIFY2(src.contains(QStringLiteral("name: \"drag-handle\"")),
                 "the grip must be the semantic drag-handle AppIcon");
    }

    // Queue item 030: the recorder page's controls row (record/freeze/elapsed/
    // folder) must be the FIRST thing on the page with the waveform immediately
    // under it — the two things you watch while recording. Config rows (mic,
    // system audio) come after. Order-only, so a source scan is the right tool.
    void recorderPage_controlsRowIsAboveWaveformAndConfigRows()
    {
        // The recorder screen was extracted out of MainPanel into its own
        // RecorderPage.qml component (2026-07-19 decomposition), so its layout
        // lives there now, not in MainPanel.qml.
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/RecorderPage.qml")));
        const QString block = enclosingBlock(src, QStringLiteral("id: recorderContent"));
        QVERIFY(!block.isEmpty());

        const int controls = block.indexOf(QStringLiteral("id: recordToggleButton"));
        const int waveform = block.indexOf(QStringLiteral("id: waveformCanvas"));
        const int mic = block.indexOf(QStringLiteral("id: micToggle"));
        const int sysAudio = block.indexOf(QStringLiteral("id: systemAudioToggle"));

        QVERIFY(controls >= 0);
        QVERIFY(waveform >= 0);
        QVERIFY(mic >= 0);
        QVERIFY(sysAudio >= 0);

        QVERIFY2(controls < waveform,
                 "record/freeze/timestamp row must be declared before the waveform Canvas");
        QVERIFY2(waveform < mic && waveform < sysAudio,
                 "the waveform must sit directly under the controls row, above the mic/system config rows");
    }

    // Queue item 030: nested layouts default Layout.fillHeight to true; without
    // this the config rows soak up the leftover height and the waveform drifts
    // away from the controls row above it. Queue item 060 moved recorderContent
    // inside a ScrollPage: ScrollView content sizes to its IMPLICIT height, so
    // rows pack to the top on their own and the old trailing fillHeight spacer
    // (which would contribute 0 there anyway) is deliberately GONE — a
    // fillHeight item inside scrollable content is dead weight at best and a
    // layout trap at worst.
    void recorderPage_rowsDoNotAbsorbLeftoverHeight()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/RecorderPage.qml")));
        const QString block = enclosingBlock(src, QStringLiteral("id: recorderContent"));
        QVERIFY(!block.isEmpty());
        QCOMPARE(block.count(QStringLiteral("Layout.fillHeight: false")), 3);
        QVERIFY2(!block.contains(QStringLiteral("Layout.fillHeight: true")),
                 "no fillHeight item belongs inside recorderContent now that it is "
                 "ScrollPage content — implicit-height sizing already packs rows to the top");
    }

    // Queue item 031: the recorder page must SHOW its destination, not just
    // offer a picker — otherwise there is no way to tell where the next
    // recording lands.
    void recorderPage_showsRecordingDestination()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/RecorderPage.qml")));
        const QString block = enclosingBlock(src, QStringLiteral("id: recorderContent"));
        QVERIFY(!block.isEmpty());

        const QString label = enclosingBlock(block, QStringLiteral("id: recordingDestinationLabel"));
        QVERIFY2(!label.isEmpty(),
                 "recorder page needs a visible destination-folder label");
        QVERIFY2(label.contains(QStringLiteral("AppSettings.recordingOutputFolder")),
                 "the destination label must read the same setting startRecording() consumes");
        QVERIFY2(label.contains(QStringLiteral("Vault root")),
                 "an empty recordingOutputFolder means the vault root and must say so, not render blank");
    }

    // Queue item 032: the waveform must plot DISPLAY levels (RMS through
    // displayLevelFromRms), never raw RMS — raw RMS pins conversational speech
    // to the bottom tenth of the canvas, which is the "too flat" regression.
    void recorderWaveform_plotsDisplayLevelsNotRawRms()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/RecorderPage.qml")));
        const QString page = enclosingBlock(src, QStringLiteral("id: recorderPage"));
        QVERIFY(!page.isEmpty());
        QVERIFY2(page.contains(QStringLiteral("audioRecorder.displayLevel(")),
                 "level samples must be mapped through AudioRecorder::displayLevel()");

        const QString canvas = enclosingBlock(page, QStringLiteral("id: waveformCanvas"));
        QVERIFY(!canvas.isEmpty());
        QVERIFY2(!canvas.contains(QStringLiteral("height - samples[i] * height")),
                 "the old baseline-anchored linear map must not come back");
        QVERIFY2(canvas.contains(QStringLiteral("const mid = height / 2")),
                 "the waveform must be mirrored about the vertical centre");
    }

    // Queue item 035: an active capture must be visible from EVERY page, not
    // just the recorder's. Two different booleans are spelled "recording" —
    // the titlebar's page-open flag and AudioRecorder's actual capture state —
    // and this indicator must key off the capture one. Guarding the wiring,
    // since a live capture needs a real audio device.
    void recordingActive_colorsTitlebarIconAndWindowRim()
    {
        const QString settings = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/AppSettings.qml")));
        QVERIFY2(settings.contains(QStringLiteral("property color recordingActiveColor")),
                 "AppSettings must expose a configurable recording-indicator color");
        QVERIFY2(settings.contains(QStringLiteral("recordingActiveColor:")),
                 "recordingActiveColor must be in the defaults map so it resets/persists");

        const QString main = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml")));
        QVERIFY2(main.contains(QStringLiteral("recordingActive: audioRecorder.recording")),
                 "the titlebar's recordingActive must come from AudioRecorder's capture state, "
                 "NOT from mainPanel.recording (which only means the recorder page is open)");
        QVERIFY2(main.contains(QStringLiteral("AppSettings.recordingActiveColor")),
                 "the window rim must take the recording-indicator color while capturing");

        const QString bar = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/TitleBar.qml")));
        const QString button = enclosingBlock(bar, QStringLiteral("id: recordButton"));
        QVERIFY(!button.isEmpty());
        QVERIFY2(button.contains(QStringLiteral("titleBar.recordingActive ? AppSettings.recordingActiveColor")),
                 "the titlebar record glyph must turn the recording-indicator color while capturing");
    }

    // Queue item 037: the three top-level-screen flags must be written ONLY via
    // MainPanel.setTopLevelScreen(), so "two screens open at once" is not a
    // representable state. Main.qml's TitleBar handlers were the only writers
    // and implemented exclusivity ad hoc; they must route through the funnel.
    void topLevelScreenFlags_writtenOnlyBySetTopLevelScreen()
    {
        const QString main = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml")));
        QVERIFY2(!main.contains(QStringLiteral("mainPanel.recording =")),
                 "recording must not be assigned directly in Main.qml — route via setTopLevelScreen() (item 037)");
        QVERIFY2(!main.contains(QStringLiteral("mainPanel.usbGrabberOpen =")),
                 "usbGrabberOpen must not be assigned directly in Main.qml — route via setTopLevelScreen() (item 037)");
        QVERIFY2(!main.contains(QStringLiteral("mainPanel.benchmarkOpen =")),
                 "benchmarkOpen must not be assigned directly in Main.qml — route via setTopLevelScreen() (item 037)");
        QVERIFY(main.contains(QStringLiteral("setTopLevelScreen(")));
        QVERIFY(main.contains(QStringLiteral("\"recorder\"")));
        QVERIFY(main.contains(QStringLiteral("\"usbGrabber\"")));
        QVERIFY(main.contains(QStringLiteral("\"benchmark\"")));
    }

    // Queue item 037: setTopLevelScreen() must assign EVERY screen flag, so
    // opening one screen closes all the others. This is what catches a future
    // fourth screen whose flag someone forgot to clear here.
    void setTopLevelScreen_assignsEveryScreenFlag()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        const QString body = functionBody(src, QStringLiteral("function setTopLevelScreen(name)"));
        QVERIFY2(!body.isEmpty(), "MainPanel must define setTopLevelScreen(name)");
        QVERIFY(body.contains(QStringLiteral("mainPanel.recording =")));
        QVERIFY(body.contains(QStringLiteral("mainPanel.usbGrabberOpen =")));
        QVERIFY(body.contains(QStringLiteral("mainPanel.benchmarkOpen =")));
    }

    // Queue item 037: with exclusivity guaranteed at the writer, page bindings
    // must not re-derive a precedence chain — the sanctioned expression is
    // mainPanel.topLevelScreenOpen.
    void topLevelPages_dontRederivePrecedence()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.contains(QStringLiteral("!mainPanel.recording")),
                 "page bindings must read mainPanel.topLevelScreenOpen, not a hand-rolled "
                 "!recording && !usbGrabberOpen && !benchmarkOpen chain (item 037)");
        QVERIFY(src.contains(QStringLiteral("topLevelScreenOpen")));
    }

    // Queue item 046: both sidebar filter fields must share ONE autocomplete
    // component (FilterSuggestions), over the same fragment logic — the two
    // filters diverging (tags had a popup, speakers didn't) is the bug this
    // replaced. Structural claim, no runtime test states it directly.
    void sidebarFilters_shareOneSuggestionComponent()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        QVERIFY2(src.count(QStringLiteral("FilterSuggestions {")) == 2,
                 "both the tag and speaker filter fields must host a FilterSuggestions popup (item 046)");
        QVERIFY2(src.contains(QStringLiteral("sidebar.tagSuggestions()"))
                     && src.contains(QStringLiteral("sidebar.speakerSuggestions()")),
                 "both filters must feed their FilterSuggestions from the sidebar suggestion helpers");
        QVERIFY2(src.contains(QStringLiteral("fragmentSuggestions(")),
                 "the tag/speaker suggestion helpers must share one fragmentSuggestions() implementation");

        const QString component = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/FilterSuggestions.qml")));
        QVERIFY2(!component.isEmpty(), "qml/FilterSuggestions.qml must exist");
        QVERIFY2(component.contains(QStringLiteral("modal: false")),
                 "the filter autocomplete must be non-modal — it must not dim its own field (item 046)");
    }

    // Queue item 043: the window-wide Ctrl+Y redo alias only exists if Main.qml
    // instantiates it — the component's own harness (tst_textredoshortcut)
    // can't see that.
    void mainWindowInstantiatesTextRedoShortcut()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml")));
        QVERIFY2(src.contains(QStringLiteral("TextRedoShortcut {")),
                 "Main.qml must instantiate TextRedoShortcut so Ctrl+Y redoes in text boxes");
    }

    // Queue item 039: the file tree must preserve its scroll offset across a
    // model reset — onModelReset latches the target into pendingContentY, which
    // expandAllNow() restores afterwards. Guarding the wiring; the actual
    // restore behaviour is covered by tst_sidebar_scroll.
    void sidebarTree_preservesScrollOffsetAcrossReset()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        // The searchProxy onModelReset latches the offset (there is a second,
        // unrelated onModelReset on sidebar.model, so match the specific line).
        QVERIFY2(src.contains(QStringLiteral("tree.pendingContentY = tree.lastUserContentY")),
                 "the model-reset handler must latch the scroll offset into pendingContentY (item 039)");
        QVERIFY2(src.contains(QStringLiteral("function restorePendingContentY()")),
                 "Sidebar must define restorePendingContentY() to put the offset back after re-expand");
    }

    // Item: renaming a tree row must not flash the old name — the row shows an
    // optimistic new name over the deferred-write window.
    void sidebar_renameHasOptimisticAntiFlash()
    {
        const QString src = sidebarTreeSrc();
        QVERIFY2(src.contains(QStringLiteral("optimisticRenamePath")),
                 "the inline rename must carry an optimistic display name to avoid the old-name flash");
        QVERIFY2(src.contains(QStringLiteral("sidebar.optimisticRenamePath === treeDelegate.path")),
                 "rowNameText must show the optimistic name for the row being renamed");
    }

    // Item: top-level pages (recorder/usb/benchmark) show a persistent active
    // border on their toolbar button and add a " - <Page>" window-title suffix.
    void titleBar_activePageBorderAndTitleSuffix()
    {
        const QString titleBar = readAll(QStringLiteral(SUOP_QML_DIR "/TitleBar.qml"));
        QVERIFY2(titleBar.contains(QStringLiteral("active: titleBar.benchmarkOpen")),
                 "the benchmark button must show an active border while its page is open");
        QVERIFY2(titleBar.contains(QStringLiteral("active: titleBar.recording"))
                     && titleBar.contains(QStringLiteral("active: titleBar.usbGrabberOpen")),
                 "the recorder and usb buttons must show an active border too");
        const QString main = readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml"));
        QVERIFY2(main.contains(QStringLiteral(" - Benchmark"))
                     && main.contains(QStringLiteral(" - Recorder"))
                     && main.contains(QStringLiteral(" - USB Grabber")),
                 "the window title must gain a \" - <Page>\" suffix for each top-level page");
    }

    // Iconography (DESIGN.md "Iconography"): AppIcon is THE way this app draws
    // an icon. Call sites name an icon semantically and never mention a glyph,
    // file, or pack — which pack supplies it is IconTheme's decision, bound to
    // AppSettings.iconPack + AppSettings.iconOverrides so it re-resolves live.
    // This contract is what makes packs swappable; the render-path tests live
    // in tst_icontheme.cpp (real QQuickView), these are the source guards.
    void appIcon_componentContract()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/AppIcon.qml"));
        QVERIFY2(!src.isEmpty(), "qml/AppIcon.qml must exist (the single icon-drawing component)");
        // Resolution must be the pure-function form reading BOTH settings, or
        // switching packs / per-icon overrides won't re-resolve every icon.
        QVERIFY2(src.contains(QStringLiteral("IconTheme.resolve(")),
                 "AppIcon must resolve via IconTheme.resolve(), not hardcode any glyph/file");
        QVERIFY(src.contains(QStringLiteral("AppSettings.iconPack")));
        QVERIFY(src.contains(QStringLiteral("AppSettings.iconOverrides")));
        // All three render paths must be present (glyph packs, svg packs, and
        // the builtin drawn shapes that keep classic's play/pause alive).
        QVERIFY2(src.contains(QStringLiteral("\"svg\"")) && src.contains(QStringLiteral("\"glyph\""))
                     && src.contains(QStringLiteral("\"builtin\"")),
                 "AppIcon must handle all three icon kinds: glyph, svg, builtin");
        // The builtin set must be a closed map, never an arbitrary URL a pack
        // could name (see AppIcon.qml's comment).
        QVERIFY2(src.contains(QStringLiteral("PlayGlyph")) && src.contains(QStringLiteral("PauseGlyph")),
                 "AppIcon's builtins map must list the drawn play/pause shapes");
    }

    // Every UI surface draws its icons through AppIcon — nobody hand-rolls a
    // one-off glyph Text or Canvas icon anymore (that drift, plus the
    // system-font-dependent glyph fragility, is exactly what the pack system
    // replaced). Count-based like the HoverBorder convention guard below, so
    // it stays robust as icons are added/removed.
    void iconography_appIconUsedOnEveryIconSurface()
    {
        for (const QString &file : {QStringLiteral("MainPanel.qml"), QStringLiteral("Sidebar.qml"),
                                    QStringLiteral("TitleBar.qml"), QStringLiteral("BottomBar.qml"),
                                    QStringLiteral("SettingsPopup.qml")}) {
            const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/") + file);
            QVERIFY2(src.contains(QStringLiteral("AppIcon")),
                     qPrintable(file + " draws icons and so must use AppIcon (see DESIGN.md Iconography)"));
        }
        // The play/pause icon that used to be a bespoke Canvas in MainPanel is
        // now an AppIcon resolving to the PlayGlyph/PauseGlyph builtins — the
        // hand-drawn shapes moved into their own reusable components.
        const QString mainPanel = mainPanelSrc();
        QVERIFY2(mainPanel.contains(QStringLiteral("? \"pause\" : \"play\"")),
                 "MainPanel's play/pause must be an AppIcon selecting the pause/play name, not a Canvas");
        for (const QString &comp : {QStringLiteral("PlayGlyph.qml"), QStringLiteral("PauseGlyph.qml")}) {
            const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/") + comp);
            QVERIFY2(src.contains(QStringLiteral("Canvas")),
                     qPrintable(comp + " must be the drawn-shape builtin (a Canvas)"));
        }
    }

    // The two settings that drive icon resolution must round-trip through the
    // vault like every other appearance setting: iconPack in `defaults`,
    // iconOverrides handled explicitly (a map, so it can't use the single-
    // value reset machinery — same carve-out as combineRules).
    void iconography_settingsPersisted()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/AppSettings.qml"));
        QVERIFY2(src.contains(QStringLiteral("property string iconPack")),
                 "AppSettings must expose iconPack");
        QVERIFY2(src.contains(QStringLiteral("property var iconOverrides")),
                 "AppSettings must expose iconOverrides");
        QVERIFY2(src.contains(QStringLiteral("iconPack:")),
                 "iconPack must be in the defaults map so it resets/persists");
        QVERIFY2(src.contains(QStringLiteral("iconOverrides = loaded.iconOverrides")),
                 "iconOverrides must be loaded explicitly from the vault (like combineRules)");
        QVERIFY2(src.contains(QStringLiteral("data.iconOverrides")),
                 "iconOverrides must be saved back to the vault");
    }

    // StyleGuide.md "Text fields": three opacity tiers, each dimmer than the
    // state above it — focused (1) > hovered (0.5) > at rest (0.25). The
    // resting tier is what makes a pure text box look like an input before
    // it's touched; only an always-editable field (plainUntilHovered) drops
    // to 0 there. Source-scan since there's no QQuickView here to drive a
    // real HoverHandler.
    void underlineField_dimsThroughThreeTiers()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/UnderlineField.qml"));
        QVERIFY2(!src.isEmpty(), "qml/UnderlineField.qml must exist");
        QVERIFY2(src.contains(QStringLiteral("HoverHandler")),
                 "UnderlineField must track hover state to preview the underline before focus");
        QVERIFY2(src.contains(QStringLiteral("property bool plainUntilHovered")),
                 "UnderlineField must expose plainUntilHovered so an always-editable field can opt "
                 "out of the resting underline");
        QVERIFY2(src.contains(QStringLiteral(
                     "focused ? 1 : (hoverHandler.hovered ? 0.5 : (plainUntilHovered ? 0 : 0.25))")),
                 "StyleGuide.md \"Text fields\": a pure text box keeps a dim 25% underline at rest "
                 "(hover 50%, focus 100%); only plainUntilHovered hides it entirely");
    }

    // StyleGuide.md "Text fields": plainUntilHovered belongs to the
    // always-editable prose fields ONLY. A pure text box (the USB grabber's
    // filename regex / start folder / extension filter) has no prose to blend
    // into, so hiding its underline at rest just makes it read as a label.
    void underlineField_onlyAlwaysEditableFieldsGoPlain()
    {
        // mainPanelSrc(), not the raw file: the prelude field lives in
        // PreludeEditor.qml since its extraction (the shim appends it).
        const QString mainPanel = mainPanelSrc();
        QVERIFY2(!mainPanel.isEmpty(), "qml/MainPanel.qml must exist");

        // Every always-editable field must opt out (StyleGuide.md
        // "Always-editable text").
        for (const char *field : {"titleField", "tagField", "tagAddField", "speakerNameField",
                                  "preludeArea", "transcriptArea"}) {
            const QString expected = QStringLiteral(
                    "UnderlineField { implicitHeight: 0; plainUntilHovered: true; focused: %1.activeFocus }")
                    .arg(QLatin1StringView(field));
            QVERIFY2(mainPanel.contains(expected),
                     qPrintable(QStringLiteral("always-editable %1 must set plainUntilHovered: true "
                                               "so it reads as plain text at rest")
                                        .arg(QLatin1StringView(field))));
        }

        // ...and no pure text box may. These USB-grabber fields moved into
        // UsbGrabberPage.qml (item 068 extraction).
        const QString usbPage = readAll(QStringLiteral(SUOP_QML_DIR "/UsbGrabberPage.qml"));
        QVERIFY2(!usbPage.isEmpty(), "qml/UsbGrabberPage.qml must exist");
        for (const char *field : {"catchAllStartFolderField", "deviceStartFolderField",
                                  "catchAllExtensionField", "deviceExtensionField"}) {
            const QString expected = QStringLiteral("UnderlineField { focused: %1.activeFocus }")
                    .arg(QLatin1StringView(field));
            QVERIFY2(usbPage.contains(expected),
                     qPrintable(QStringLiteral("pure text box %1 must keep its resting underline "
                                               "(no plainUntilHovered)")
                                        .arg(QLatin1StringView(field))));
        }

        const QString regexField = readAll(QStringLiteral(SUOP_QML_DIR "/UsbRegexField.qml"));
        QVERIFY2(!regexField.isEmpty(), "qml/UsbRegexField.qml must exist");
        QVERIFY2(regexField.contains(
                         QStringLiteral("UnderlineField { focused: patternField.activeFocus }")),
                 "the filename-regex field is a pure text box — it must keep its resting underline");
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
        // mainPanelSrc(), not the raw file: preludeArea lives in
        // PreludeEditor.qml since its extraction (the shim appends it).
        const QString src = mainPanelSrc();
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

    // Perf (UI lag fix): job-status ticks must NOT trigger refreshTreeOrdering().
    //
    // Tree ORDER is a pure function of the filesystem + settings
    // (allAudioPaths / detectCombineGroups / audioLastModified /
    // AppSettings.sortMode); job status is queue state (queued/active/
    // paused/...) and cannot change any of them, so the call only ever
    // recomputed an identical answer. It was expensive — a full vault walk, a
    // QFileInfo stat per combine-rule per audio file inside
    // findCombineGroups(), another stat sweep from audioLastModified(), a JS
    // sort, and then a fileSortKeys assignment that invalidates the proxy's
    // whole O(n log n) sort — and it fired on every status tick, i.e.
    // constantly during a transcription queue run, exactly when the app is
    // already busy.
    //
    // The legitimate triggers must stay: onFinished (a job's write really can
    // change ordering, e.g. a combine group becoming alreadyTranscribed) and
    // onModelReset (the watcher saw a directory change).
    void sidebar_jobStatusTicksDoNotRebuildTreeOrdering()
    {
        // Comments stripped FIRST: this handler's body deliberately documents
        // at length why it does NOT call refreshTreeOrdering(), so a raw scan
        // would find the call text in that very explanation and fail.
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        const QString handler = functionBody(src, QStringLiteral("function onJobStatusesChanged()"));
        QVERIFY2(!handler.isEmpty(), "Sidebar.qml must have an onJobStatusesChanged handler");
        QVERIFY2(handler.contains(QStringLiteral("jobStatusRevision += 1")),
                 "onJobStatusesChanged must still nudge delegates via jobStatusRevision");
        QVERIFY2(!handler.contains(QStringLiteral("refreshTreeOrdering()")),
                 "onJobStatusesChanged must NOT call refreshTreeOrdering() — tree order does not "
                 "depend on queue status, and the call re-walks + re-stats the whole vault and "
                 "re-runs the proxy's sort on every status tick");

        // The real triggers must survive. Prefix lookup, not the full parameter
        // list — the signature gained a 4th `structural` param (queue item 021)
        // and an exact-literal lookup silently stopped finding the function.
        const QString finished = functionBody(src, QStringLiteral("function onFinished("));
        QVERIFY2(!finished.isEmpty(), "Sidebar.qml must have an onFinished handler");
        // Still called for a NON-structural finish (an in-place label/tags/
        // chapters write, which has no watcher reset of its own). Item 021 only
        // gates it behind !structural; it must not disappear entirely.
        QVERIFY2(finished.contains(QStringLiteral("refreshTreeOrdering()")),
                 "onFinished must still call refreshTreeOrdering() for a non-structural finish — "
                 "an in-place write can change ordering and nothing else reflows for it");
    }

    // Perf convention (UI lag fix): the transcript's four overlay Repeaters
    // must take a PRE-FILTERED model, never raw displaySentences with the
    // unwanted entries hidden by `visible:`.
    //
    // A Repeater instantiates every delegate its model has; `visible: false`
    // only skips painting one that already exists. Pointing all four at
    // displaySentences built 4 delegates per sentence — 16k QQuickItems on a
    // 4k-sentence transcript, synchronously on the GUI thread, the instant a
    // file was opened — and three of the four were near-100% waste (the dim
    // overlay is gated on gutterSpeakerFilter, which resets to "" on every
    // open; chapter headings show only at real markers; the speaker gutter
    // needs speaker !== ""). Measured on an offscreen reproduction of this
    // exact structure: 661ms -> 130ms for a 4k-sentence cold open.
    //
    // This is NOT fixable by threading — QQuickItems can only be created on
    // the GUI thread (the file read + parse were already moved off it by item
    // P23 / loadTranscriptAsync). So the only lever is not creating them, and
    // a regression here is invisible until someone opens a big file. Hence a
    // source guard.
    void mainPanel_transcriptOverlayRepeatersUsePreFilteredModels()
    {
        // Comments stripped: the models carry a long explanation of the very
        // anti-patterns asserted against below, so these must scan code only.
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        // Each overlay Repeater -> the filtered model property it must use.
        const QList<QPair<QString, QString>> repeaterModels{
            {QStringLiteral("id: timestampGutterRepeater"), QStringLiteral("mainPanel.gutterTimestampModel")},
            {QStringLiteral("id: speakerGutterRepeater"), QStringLiteral("mainPanel.gutterSpeakerModel")},
            {QStringLiteral("id: speakerDimOverlayRepeater"), QStringLiteral("mainPanel.speakerDimModel")},
            {QStringLiteral("id: chapterHeadingRepeater"), QStringLiteral("mainPanel.chapterHeadingModel")},
        };
        for (const auto &[anchor, expectedModel] : repeaterModels) {
            const QString block = enclosingBlock(src, anchor);
            QVERIFY2(!block.isEmpty(), qPrintable(QStringLiteral("%1 must exist in MainPanel.qml").arg(anchor)));
            QVERIFY2(block.contains(QStringLiteral("model: ") + expectedModel),
                     qPrintable(QStringLiteral("%1 must use the pre-filtered `model: %2` — a Repeater builds a "
                                               "delegate for EVERY model entry, so a raw displaySentences model "
                                               "instantiates one per sentence no matter what `visible:` says")
                                    .arg(anchor, expectedModel)));
            QVERIFY2(!block.contains(QStringLiteral("model: mainPanel.displaySentences")),
                     qPrintable(QStringLiteral("%1 must not take raw displaySentences as its model").arg(anchor)));
        }

        // The four filtered models must actually exist and each narrow
        // displaySentences down, rather than alias it.
        for (const QString &prop : {QStringLiteral("gutterTimestampModel"), QStringLiteral("gutterSpeakerModel"),
                                    QStringLiteral("speakerDimModel"), QStringLiteral("chapterHeadingModel")}) {
            const QString decl = QStringLiteral("readonly property var ") + prop;
            QVERIFY2(src.contains(decl), qPrintable(QStringLiteral("MainPanel.qml must declare %1").arg(decl)));
        }
        QVERIFY2(src.count(QStringLiteral("displaySentences.filter(")) >= 3,
                 "the filtered overlay models must genuinely filter displaySentences");

        // Filtered delegates index gutterLineRects by the source index carried
        // on the entry (modelData.idx), never by the delegate's own `index` —
        // that is now its position in the FILTERED list, so using it would
        // silently misalign every gutter with the wrong transcript line.
        QVERIFY2(src.contains(QStringLiteral("gutterLineRects[modelData.idx]"))
                     || src.contains(QStringLiteral("gutterLineRects[chapterHeadingRow.modelData.idx]")),
                 "overlay delegates must look up gutterLineRects by modelData.idx");
        QVERIFY2(!src.contains(QStringLiteral("gutterLineRects[index]")),
                 "gutterLineRects must never be indexed by a filtered delegate's own `index` — "
                 "use the entry's own modelData.idx (assigned in displayData)");
    }

    // Item 33 regression: collapsing a chapter appeared to do nothing to the
    // transcript body — it moved the following heading and gutter timestamps
    // up (those are bound to displaySentences, which recomputes) while the
    // collapsed sentences stayed on screen, so the heading landed on top of
    // text it was supposed to have hidden.
    //
    // The cause is that transcriptArea's text is NOT a live binding:
    // onDisplayTextChanged assigns it (severing the declarative one on the
    // first change) and skips the assignment while the area has focus, so an
    // edit in progress is never clobbered. The collapse triangle is an overlay
    // ON the transcript, so by the time it's tapped transcriptArea is very
    // often already focused — and the resync silently never happens.
    // toggleChapterCollapse() must therefore blur first (which also commits
    // any pending edit via onActiveFocusChanged).
    void mainPanel_collapsingAChapterBlursTranscriptSoTheBodyResyncs()
    {
        // Comments stripped: the function carries the explanation above, which
        // would otherwise satisfy the scan on prose alone.
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const QString body = functionBody(src, QStringLiteral("function toggleChapterCollapse"));
        QVERIFY2(!body.isEmpty(), "MainPanel.qml must declare toggleChapterCollapse()");
        QVERIFY2(body.contains(QStringLiteral("transcriptArea.focus = false")),
                 "toggleChapterCollapse() must blur transcriptArea before changing collapsedChapters — "
                 "onDisplayTextChanged skips the text resync while it has focus, so without this the "
                 "collapse moves the overlays but leaves the collapsed body text on screen");

        // The blur only matters because the resync is focus-gated; if that
        // gate ever goes away this guard should go with it, not linger as
        // cargo cult.
        QVERIFY2(src.contains(QStringLiteral("onDisplayTextChanged: if (!transcriptArea.activeFocus)")),
                 "the transcript resync is expected to be focus-gated — if that changed, revisit "
                 "toggleChapterCollapse()'s blur (this test) rather than leaving both in place");
    }

    // Item 35 regression: clicking to the right of a chapter heading put an
    // edit cursor on the heading's own blank reserved line (a line with no
    // editable content) instead of editing the chapter title.
    //
    // Every chapter heading is an overlay row sitting ON transcriptArea, so
    // any part of it that doesn't accept the press itself leaks that press
    // through to the TextArea underneath. The collapse triangle is an AppIcon
    // (a plain Item — accepts no mouse of its own) and a TapHandler's default
    // DragThreshold policy takes only a PASSIVE grab, so its tap leaked too.
    //
    // The space right of the title was once a sibling strip with its own tap,
    // which stopped the leak but made that space a BUTTON rather than part of
    // the field: a click past the last character while editing did nothing, and
    // a drag from out there selected nothing (its tap grabbed the press; a tap
    // can begin an edit but can't extend a selection). headingTitle now fills
    // that space itself, which both plugs the leak and makes the whole heading
    // line real, clickable, drag-selectable text.
    void mainPanel_chapterHeadingRowClicksNeverLandInTheTranscript()
    {
        const QString src = stripComments(mainPanelSrc());
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const QString row = enclosingBlock(src, QStringLiteral("id: chapterHeadingRow"));
        QVERIFY2(!row.isEmpty(), "id: chapterHeadingRow must exist in MainPanel.qml");

        // The collapse triangle is an AppIcon (a plain Item — it accepts no
        // mouse buttons of its own), so its tap MUST take an exclusive grab.
        const QString triangleTap = enclosingBlock(
            row, QStringLiteral("mainPanel.toggleChapterCollapse(chapterHeadingRow.modelData.chapterT0)"));
        QVERIFY2(triangleTap.contains(QStringLiteral("gesturePolicy: TapHandler.ReleaseWithinBounds")),
                 "the collapse triangle's TapHandler must set an exclusive-grab gesturePolicy — the "
                 "default DragThreshold is a passive grab, so the press also reaches transcriptArea "
                 "underneath and focuses it");

        // Clicking anywhere along the heading edits its title, because the
        // title's field IS the rest of the heading line.
        QVERIFY2(row.contains(QStringLiteral("function beginTitleEdit")),
                 "chapterHeadingRow must expose beginTitleEdit() (the at-rest click-to-edit entry point)");
        const QString title = enclosingBlock(row, QStringLiteral("id: headingTitle"));
        QVERIFY2(!title.isEmpty(), "id: headingTitle must exist in chapterHeadingRow");
        QVERIFY2(title.contains(QStringLiteral("Layout.fillWidth: true")),
                 "headingTitle must fill the heading line's width — the space right of the title has to "
                 "be part of the FIELD (so a click/drag past the last character reaches the text), and a "
                 "TextEdit accepts the press itself, which is what keeps it off transcriptArea");

        // The trailing rule must not be a sibling tap target again: it belongs
        // INSIDE the field, offset by the text's width.
        QVERIFY2(!row.contains(QStringLiteral("Layout.fillHeight: true")),
                 "the space right of the title must not be a separate layout item — that's the strip "
                 "whose tap swallowed clicks/drags past the last character");
        QVERIFY2(title.contains(QStringLiteral("headingTitle.contentWidth")),
                 "the heading rule must be a child of headingTitle positioned past contentWidth, not a "
                 "sibling laid out beside it");
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
        // The pure pattern-analysis functions were lifted into CombineRuleRegex.js
        // (a .pragma library module); the QML wiring that calls them stays in
        // SettingsPopup.qml. Scan both so each assertion finds the machinery
        // wherever it now lives.
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"))
                            + readAll(QStringLiteral(SUOP_QML_DIR "/CombineRuleRegex.js"));
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

    // The USB grab rules' filename-regex field (UsbGrabRule::filenameRegex),
    // which deliberately mirrors the combine rules' pattern authoring UI
    // above. Source-scan guard for the same reason: no QQuickView in this
    // suite to drive the live bindings.
    void usbGrabber_filenameRegexAuthoringAids()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/UsbRegexField.qml"));
        QVERIFY2(!src.isEmpty(), "qml/UsbRegexField.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("controller.regexValidationError(")),
                 "the pattern field's live validity indicator must call "
                 "UsbGrabberController::regexValidationError() (the real QRegularExpression engine "
                 "UsbGrabJob filters with), not a QML/JS RegExp re-implementation — the two "
                 "syntaxes aren't guaranteed identical, so a JS-valid pattern could still match "
                 "nothing at grab time");
        QVERIFY2(!src.contains(QStringLiteral("new RegExp")),
                 "must not validate or match with JS RegExp anywhere in this field, for the same "
                 "reason");

        QVERIFY2(src.contains(QStringLiteral("focusPolicy: Qt.NoFocus")),
                 "every insert-token button needs focusPolicy: Qt.NoFocus — a click's press steals "
                 "the pattern field's focus, firing onEditingFinished -> commitRuleField(), which "
                 "re-reads the rule store and reassigns deviceRows, destroying the very button "
                 "mid-click before onClicked can fire");
        QVERIFY2(src.contains(QStringLiteral("patternField.insert(")),
                 "tokens must be inserted via TextInput.insert(), not a `text = ...` reassignment "
                 "— reassigning the whole text property silently clears TextInput's undo history, "
                 "so Ctrl+Z would stop working after a token click");

        // An invalid pattern fails closed in UsbGrabJob (matches nothing), so
        // the field must say so rather than leaving an all-"filtered" preview
        // as the only clue.
        QVERIFY2(src.contains(QStringLiteral("will grab nothing until the pattern is valid")),
                 "an invalid pattern matches nothing at grab time — the field must warn that the "
                 "rule currently grabs nothing, not just that the regex is malformed");

        // Both rule cards must offer it: the "*" catch-all and every
        // per-device card are the two places a rule is edited, and a filter
        // that exists on only one of them is an incomplete feature.
        // The USB grabber screen lives in UsbGrabberPage.qml (extracted from
        // MainPanel, 2026-07-19 decomposition).
        const QString panel = readAll(QStringLiteral(SUOP_QML_DIR "/UsbGrabberPage.qml"));
        QVERIFY2(panel.count(QStringLiteral("UsbRegexField {")) >= 2,
                 "both the \"Any USB device\" catch-all card and the per-device card must expose "
                 "the filename-regex field");
        QVERIFY2(panel.contains(QStringLiteral("\"filenameRegex\", newPattern")),
                 "the field must commit through commitRuleField(), the same per-field save path "
                 "every other rule field uses");
    }

    // Combine-rule "differentiator" buttons: every non-exact-match run of a
    // rule's pattern gets a clickable button that toggles whether files must
    // share that run's captured text to combine. No QQuickView in this
    // suite to drive the live bindings, so this is a source-scan guard that
    // the toggle mechanism, the participation-block check, and the
    // rule-overlap warning all still exist, rather than a behavioral test.
    void settingsPopup_combineRuleDifferentiatorButtons()
    {
        // Pure functions live in CombineRuleRegex.js; the QML wiring
        // (differentiatorNonParticipatingFiles call, blocked gating,
        // computeRuleOverlaps) stays in SettingsPopup.qml. Scan both.
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"))
                            + readAll(QStringLiteral(SUOP_QML_DIR "/CombineRuleRegex.js"));
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
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"))
                            + readAll(QStringLiteral(SUOP_QML_DIR "/CombineRuleRegex.js"));
        QVERIFY2(!src.isEmpty(), "qml/SettingsPopup.qml must exist");

        // The palette moved into CombineRuleRegex.js as a module-local const
        // (was a QML `property var`); still a fixed-order categorical list.
        QVERIFY2(src.contains(QStringLiteral("differentiatorPalette = [")),
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

        QVERIFY2(src.contains(QStringLiteral("CombineRuleRegex.differentiatorColor(modelData.groupIndex)")),
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
        // resolveSortGroupIndex() moved into CombineRuleRegex.js; the occurrence
        // threading/persistence (commitPatternChange, AppSettings.combineRules)
        // stays in SettingsPopup.qml. Scan both.
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"))
                            + readAll(QStringLiteral(SUOP_QML_DIR "/CombineRuleRegex.js"));
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

    // Settings → Models page: a dedicated "Models" nav page (ModelsPage.qml)
    // must exist, mounted by SettingsPopup, wiring the model-management
    // invokables (manageableModels / downloadModel / deleteModel / importModel
    // / exportModel). No QQuickView here to click the page, so this is a
    // source-scan guard that the page and its controller wiring all exist.
    void settingsPopup_hasModelsManagementPage()
    {
        const QString settings = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(settings.contains(QStringLiteral("ModelsPage")),
                 "SettingsPopup must mount the ModelsPage component");
        QVERIFY2(settings.contains(QStringLiteral("qsTr(\"Models\")")),
                 "the settings nav rail must include a \"Models\" page");

        const QString page = readAll(QStringLiteral(SUOP_QML_DIR "/ModelsPage.qml"));
        QVERIFY2(!page.isEmpty(), "qml/ModelsPage.qml must exist");
        for (const char *invokable :
             {"manageableModels(", "downloadModel(", "deleteModel(", "importModel(", "exportModel("}) {
            QVERIFY2(page.contains(QString::fromLatin1(invokable)),
                     qPrintable(QStringLiteral("ModelsPage must call transcriptionController.%1")
                                    .arg(QString::fromLatin1(invokable))));
        }
        // Delete of a downloaded model is permanent (re-downloadable, not user
        // content) — it must be confirmed (a model delete is one of the few
        // deletes that IS prompted; see StyleGuide "Destructive actions").
        QVERIFY2(page.contains(QStringLiteral("deleteConfirmPopup")),
                 "deleting a model must go through a confirmation popup");
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
        const QString src = sidebarTreeSrc();
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
        QVERIFY2(src.contains(QStringLiteral("sidebar.model.trashFolder(")),
                 "Delete folder must call VaultTreeModel.trashFolder() — the trash-first call. "
                 "deleteFolderPermanently() is only for the confirmed, untrashable case");
        // Move to… goes through the controller's instant-lane MoveTask when a
        // controller is present (async, undoable, visible in the Tasks list),
        // and falls back to the synchronous VaultTreeModel.moveFile() only when
        // there's no controller (plain-model harness) — same split as the
        // rename guard below.
        QVERIFY2(src.contains(QStringLiteral("sidebar.transcriptionController.moveFiles(")),
                 "Move to… (with a controller) must go through TranscriptionController.moveFiles() so it's "
                 "an instant-lane task visible in the Tasks list");
        QVERIFY2(src.contains(QStringLiteral("sidebar.model.moveFile(")),
                 "Move to… must still fall back to VaultTreeModel.moveFile() when there's no controller");
        QVERIFY2(src.contains(QStringLiteral("sidebar.model.vaultFolders()")),
                 "the Move to… picker must enumerate destinations via VaultTreeModel.vaultFolders()");
        QVERIFY2(src.contains(QStringLiteral("\"file://\" + sidebar.ctxCreateInDir"))
                     || src.contains(QStringLiteral("\"file://\" + sidebar.ctxPath")),
                 "the invokable calls must pass file:// URLs, not raw local paths");

        // Deleting a folder is the most destructive action in the tree (it takes
        // every transcript under it), so it must stay REVERSIBLE — but the
        // reversibility now comes from the trash + an Undo snackbar, not from a
        // confirmation prompt. The prompt was dropped deliberately: it fired on
        // every delete, including the recoverable ones, so it cried wolf. It now
        // appears only when the trash refuses (see handleTrashResult), which is
        // the only genuinely permanent case. What must hold:
        QVERIFY2(!src.contains(QStringLiteral("id: deleteConfirmPopup")),
                 "the folder-specific confirm popup is gone — a trashable folder delete is "
                 "recoverable and offers Undo instead; the shared fileDeleteConfirmPopup "
                 "handles the untrashable case for all three delete kinds");
        QVERIFY2(src.contains(QStringLiteral("sidebar.beginDeleteFolder(")),
                 "\"Delete folder\" must route through beginDeleteFolder(), which trashes first");
        QVERIFY2(src.contains(QStringLiteral("function handleTrashResult(")),
                 "the trash-first flow must funnel through handleTrashResult() — that's what "
                 "offers Undo for what trashed and prompts only for what didn't");

        // The empty-area handler must bail when the tap landed on a real cell.
        // Nested TapHandlers are NOT mutually exclusive, so both fire on a row
        // right-click and this one fires LAST — without the guard it overwrote
        // the row's ctx* state with the empty-area's, leaving every row's menu
        // showing only "New folder". tst_sidebar_rename's
        // rightClickRow_menuOffersRenameAndNamesTheSelectionCount proves the
        // behavior against a real view; this guards the mechanism.
        const QString emptyTapBlock = enclosingBlock(src, QStringLiteral("id: treeEmptyContextTap"));
        QVERIFY2(!emptyTapBlock.isEmpty(), "could not locate the treeEmptyContextTap block");
        QVERIFY2(emptyTapBlock.contains(QStringLiteral("cellAtPosition")),
                 "the empty-area right-click must check it didn't land on a row (cellAtPosition) — nested "
                 "TapHandlers both fire, and this one runs last, so it would clobber the row's menu state");
    }

    // Multi-select delete: "Delete file" must act on the whole selection when
    // the right-clicked row is part of one, and must SAY so rather than
    // silently deleting more rows than the menu names. No QQuickView harness
    // for TreeView context menus, so this is a source-scan guard.
    void sidebar_deleteFileActsOnWholeSelection()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        // Drag-to-move and delete must read a multi-selection through the same
        // shared rule, not each invent its own.
        QVERIFY2(src.contains(QStringLiteral("function selectionPathsFor(")),
                 "the \"row in a 2+ selection carries the whole selection\" rule must be one shared "
                 "function (selectionPathsFor)");
        // selectionPathsFor() is applied at the CALL SITE now, not inside
        // beginDeleteFile() — the ribbon's deleteFileAllButton and the Delete
        // key pass a selection subset directly (they have no right-clicked row
        // for selectionPathsFor to widen from), while the context menu widens
        // its right-clicked row through it. So the context-menu row is where
        // this rule must appear.
        const QString ctxDeleteFile = enclosingBlock(src, QStringLiteral("id: ctxDeleteFileRow"));
        QVERIFY2(!ctxDeleteFile.isEmpty(), "ctxDeleteFileRow must exist");
        QVERIFY2(ctxDeleteFile.contains(QStringLiteral("selectionPathsFor(")),
                 "\"Delete file\" must target the whole multi-selection via selectionPathsFor()");
        QVERIFY2(ctxDeleteFile.contains(QStringLiteral("beginDeleteFile(")),
                 "the context-menu \"Delete file\" row must call beginDeleteFile()");

        // The menu row must name the real count.
        QVERIFY2(ctxDeleteFile.contains(QStringLiteral("Delete %1 items")),
                 "with 2+ rows selected the menu must read \"Delete N items\", not a bare \"Delete file\" "
                 "that hides how many rows it will take");

        // The batch is ONE call for the whole selection (not a per-file QML
        // loop — that's a full vault rescan + model reset each). With a
        // controller it's the async instant-lane TrashTask
        // (transcriptionController.trash), undoable via the generic snackbar; the
        // synchronous VaultTreeModel.trashFiles() stays as the no-controller
        // fallback (same split as performRename/performRowMove/beginDeleteFolder).
        {
            const QString body = functionBody(src, QStringLiteral("function beginDeleteFile("));
            QVERIFY2(body.contains(QStringLiteral("sidebar.transcriptionController.trash(")),
                     "a delete (with a controller) must route through TranscriptionController.trash() so it's "
                     "an instant-lane task visible in the Tasks list + undoable via the generic snackbar");
            QVERIFY2(body.contains(QStringLiteral("sidebar.model.trashFiles(")),
                     "a delete must still fall back to VaultTreeModel.trashFiles() when there's no controller");
        }
        // The confirm-then-permanent path (only for entries the trash refused)
        // likewise prefers the async DeleteTask.
        QVERIFY2(functionBody(src, QStringLiteral("function confirmFileDelete("))
                     .contains(QStringLiteral("sidebar.transcriptionController.deletePermanently(")),
                 "a confirmed permanent delete (with a controller) must route through "
                 "TranscriptionController.deletePermanently()");

        // Destructive, so still confirmed — and the confirmation must be
        // count-aware too, not name a single file while deleting several.
        //
        // Brace-matched, NOT a fixed-size src.mid() window: this was a 2000-char
        // slice, which silently measures "how much QML precedes the needle
        // inside the popup" rather than the behavior it claims to assert.
        // Adding one branch to the popup's message text (the "audioOnly" mode)
        // pushed the needle to offset 1982..2011 and truncated it mid-string,
        // failing a test whose subject hadn't changed at all.
        const QString confirmBlock = enclosingBlock(src, QStringLiteral("id: fileDeleteConfirmPopup"));
        QVERIFY2(!confirmBlock.isEmpty(), "fileDeleteConfirmPopup must exist");
        QVERIFY2(confirmBlock.contains(QStringLiteral("pendingDeletePaths.length > 1")),
                 "the delete confirmation must state how many items it will delete");
    }

    // Inline rename (context menu "Rename" + clicking an already-selected row's
    // name). StyleGuide.md "Always-editable text" carve-out: this is a
    // deliberate display-vs-edit swap, but it must be gated on an explicit
    // renamingPath — NEVER on activeFocus, which is what let a focus-steal hide
    // the field out from under the user (MainPanel's old renamingTitle bug).
    void sidebar_inlineRenameIsGatedOnRenamingPathNotFocus()
    {
        const QString src = sidebarTreeSrc();
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("property string renamingPath")),
                 "inline rename must track which row is editing via a sidebar-level renamingPath — "
                 "delegate-local state would retarget itself when TreeView recycles the row");
        QVERIFY2(src.contains(QStringLiteral("id: rowRenameField")),
                 "the tree row must have an inline rename editor (rowRenameField)");

        const QString fieldBlock = enclosingBlock(src, QStringLiteral("id: rowRenameField"));
        QVERIFY2(!fieldBlock.isEmpty(), "could not locate the rowRenameField block");
        // The gate reads through treeDelegate.renamingThisRow, which must
        // itself be nothing but the renamingPath comparison — checking both
        // keeps this guard about renamingPath-vs-activeFocus rather than about
        // where the comparison is spelled.
        QVERIFY2(fieldBlock.contains(QStringLiteral("visible: treeDelegate.renamingThisRow")),
                 "the rename editor's presence must be gated on renamingThisRow, not on activeFocus");
        QVERIFY2(src.contains(QStringLiteral(
                     "readonly property bool renamingThisRow: sidebar.renamingPath === treeDelegate.path")),
                 "renamingThisRow must be defined as the explicit renamingPath comparison — it is what "
                 "the rename editor and the row's hover overlays are all gated on");
        QVERIFY2(fieldBlock.contains(QStringLiteral("background: UnderlineField")),
                 "StyleGuide.md \"Text fields\": the rename editor must use the UnderlineField background");
        // Blur commits, but only after the field actually held focus —
        // otherwise the opening frame (or a modal menu releasing focus) reads
        // as a blur and commits instantly.
        QVERIFY2(fieldBlock.contains(QStringLiteral("everFocused")),
                 "the blur-commit must be guarded so entering edit mode isn't itself read as a blur");
        QVERIFY2(fieldBlock.contains(QStringLiteral("Keys.onEscapePressed")),
                 "StyleGuide.md commit convention: Escape must abandon the rename without committing");

        // Both entry points: the context-menu row and the click-the-name-of-an-
        // already-selected-row gesture.
        QVERIFY2(src.contains(QStringLiteral("id: ctxRenameRow")),
                 "the context menu must offer a Rename row");
        QVERIFY2(src.contains(QStringLiteral("function tapIsOnRowName(")),
                 "clicking an already-selected row's name must start a rename — decided inside the row's "
                 "own TapHandler (overlapping TapHandlers both fire, racing rename against open)");

        // Renaming rebuilds the model, destroying the delegate whose handler is
        // mid-execution — same hazard the drop handler defers for.
        // Brace-balanced functionBody(), not a fixed-width src.mid() window:
        // the window had already been widened once (700 -> 1200) and broke
        // again when the rename text derivation moved to TextFormat.js and the
        // call sites grew their lifted AppSettings arguments. A char count is
        // not the property being guarded — "inside commitActiveRename()" is.
        const QString commitBody =
            functionBody(stripComments(src), QStringLiteral("function commitActiveRename("));
        QVERIFY2(!commitBody.isEmpty(), "commitActiveRename() must exist");
        QVERIFY2(commitBody.contains(QStringLiteral("Qt.callLater")),
                 "the rename commit must be deferred (Qt.callLater) — renameFile()/renameFolder() rebuild "
                 "the model and destroy the delegate the commit handler is running on");

        // The editor's text must be mirrored onto the sidebar, because the
        // commit can't always come from the editor: the TreeView is a
        // FocusScope, so clicking another row hands activeFocus back to the
        // editor instead of blurring it, and treeRowTap has to commit on its
        // behalf. tst_sidebar_rename's blur_commitsRename proves the behavior;
        // this guards the state design it depends on.
        QVERIFY2(src.contains(QStringLiteral("property string renamingText")),
                 "the editor's live text must be mirrored on the sidebar so a click on another row can "
                 "commit a rename the editor itself never gets a blur for");
        const int rowTapIdx2 = src.indexOf(QStringLiteral("id: treeRowTap"));
        QVERIFY2(rowTapIdx2 >= 0, "treeRowTap must exist");
        QVERIFY2(src.mid(rowTapIdx2, 1200).contains(QStringLiteral("commitActiveRename()")),
                 "clicking a different row while an inline rename is open must commit it — that click "
                 "never blurs the editor (FocusScope), so nothing else will");

        // Folder rows rename through the folder-specific invokable.
        // Folder rename goes through the async FolderRenameTask with a
        // controller (undoable, visible), falling back to the synchronous
        // VaultTreeModel.renameFolder() with none — same split as the file paths.
        QVERIFY2(src.contains(QStringLiteral("sidebar.transcriptionController.renameFolder(")),
                 "renaming a directory row (with a controller) must go through "
                 "TranscriptionController.renameFolder() so it's an instant-lane task");
        QVERIFY2(src.contains(QStringLiteral("sidebar.model.renameFolder(")),
                 "renaming a directory row must still fall back to VaultTreeModel.renameFolder() with no controller");
        // Note rows rename through the controller's instant-lane task when one
        // is wired (the real app — visible in the Tasks list, deferred behind a
        // same-file transcription), falling back to the synchronous
        // VaultTreeModel.renameFile() only when there's no controller (plain
        // filename mode). BOTH call sites must still exist.
        QVERIFY2(src.contains(QStringLiteral("sidebar.transcriptionController.renameFile(")),
                 "renaming a note row (with a controller) must go through TranscriptionController.renameFile() "
                 "so it's an instant-lane task visible in the Tasks list");
        QVERIFY2(src.contains(QStringLiteral("sidebar.model.renameFile(")),
                 "renaming a note row must still fall back to VaultTreeModel.renameFile() when there's no "
                 "controller (plain filename mode)");
    }

    // Item 40 (drag-to-move): dragging a tree row must move the file/folder,
    // not flick the TreeView (user report: "dragging files in the side panel
    // merely drags the whole thing, not allowing you to do any operations").
    // No QQuickView harness exists for TreeView delegate gestures, so this is a
    // source-scan guard per CLAUDE.md.
    void sidebar_dragToMoveRows()
    {
        const QString src = sidebarTreeSrc();
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        // The gesture itself: a per-row DragHandler.
        const int dragIdx = src.indexOf(QStringLiteral("id: rowDragHandler"));
        QVERIFY2(dragIdx >= 0, "each tree row must have a DragHandler so a row can be dragged to move it");
        const QString dragBlock = src.mid(dragIdx, 2000);

        // Whether the drag survives the TreeView's flick isn't source-scannable
        // — tst_sidebar_dragmove's dragDoesNotFlickTheTree covers that against
        // a real QQuickView instead.
        //
        // target: null — the row stays in the tree's layout; rowDragGhost is
        // what follows the cursor.
        QVERIFY2(dragBlock.contains(QStringLiteral("target: null")),
                 "the row DragHandler must not move the row itself (target: null) — the ghost follows "
                 "the cursor instead");

        // The drag source item + matching keys on both ends, so a row drag and
        // Main.qml's window-wide file-import DropArea can't claim each other's
        // drops.
        QVERIFY2(src.contains(QStringLiteral("id: rowDragGhost")),
                 "a drag-source/ghost item (rowDragGhost) must exist");
        QVERIFY2(src.contains(QStringLiteral("Drag.keys: [\"suopVaultRow\"]")),
                 "the drag ghost must key its drag so only the sidebar's own row DropAreas accept it");
        QVERIFY2(src.contains(QStringLiteral("id: rowDropTarget")),
                 "each tree row must be a DropArea (rowDropTarget) so a row can be dropped onto a folder");
        QVERIFY2(src.contains(QStringLiteral("id: rootDropTarget")),
                 "the tree's empty space must be a DropArea targeting the vault root — the only way to "
                 "drag a file back out of a subfolder");
        QVERIFY2(src.count(QStringLiteral("keys: [\"suopVaultRow\"]")) >= 2,
                 "both the row and root DropAreas must match the ghost's Drag.keys");

        // A drop must go through the same C++ invokables the "Move to…" menu
        // uses — including moveFolder(), which no other UI reaches (ctxMoveRow
        // is note-only).
        QVERIFY2(src.contains(QStringLiteral("sidebar.transcriptionController.moveFolder(")),
                 "dragging a directory row (with a controller) must go through "
                 "TranscriptionController.moveFolder() so it's an instant-lane task");
        QVERIFY2(src.contains(QStringLiteral("sidebar.model.moveFolder(")),
                 "dragging a directory row must still fall back to VaultTreeModel.moveFolder() with no controller");
        QVERIFY2(src.contains(QStringLiteral("sidebar.performRowMove(")),
                 "drops must route through performRowMove()");
        // A dragged FILE move goes through the instant-lane task queue (one undo
        // batch) when a controller is present — a folder move stays synchronous
        // (folder-as-task isn't built yet), and a no-controller harness falls
        // back to the synchronous model calls.
        QVERIFY2(src.contains(QStringLiteral("sidebar.transcriptionController.moveFiles(")),
                 "performRowMove must route file moves through TranscriptionController.moveFiles() when a "
                 "controller is present, so a drag-move is an instant-lane task visible in the Tasks list");

        // The move must be deferred out of the drop callback: it rebuilds the
        // model, destroying the very delegate whose handler is mid-execution
        // (this codebase has been bitten by a delegate rebuilt under an
        // in-flight click before).
        const int dropIdx = src.indexOf(QStringLiteral("id: rowDropTarget"));
        QVERIFY2(src.mid(dropIdx, 2600).contains(QStringLiteral("Qt.callLater(() => sidebar.performRowMove(")),
                 "the drop handler must defer the move (Qt.callLater) — it rebuilds the model and "
                 "destroys the delegate the handler is running on");
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

    // Item 1d (download integrity): downloadToModelsDir() must (a) reject a
    // curl-exited-0-but-truncated body by comparing the final file size against
    // the upfront Content-Length probe, and (b) check QFile::rename's return
    // value so a failed move (Qt's rename won't overwrite an existing target)
    // never reports success while orphaning the fresh .part. No unit test drives
    // this — the download path shells out to real curl over the network, which
    // tst_modelmanager.cpp deliberately never exercises ("tests must pass with
    // no network") — so it's guarded at the source. Both tokens are unique to
    // this function (verified). functionBody() can't anchor here because the
    // signature's `= {}` default args trip its brace matcher, so this scans the
    // comment-stripped file for the two specific tokens.
    void modelManager_downloadVerifiesSizeAndCheckedRename()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_SRC_DIR "/core/ModelManager.cpp")));
        QVERIFY2(src.contains(QStringLiteral("finalSize != totalBytes")),
                 "downloadToModelsDir must reject a truncated download by comparing the final "
                 "size against the Content-Length probe (item 1d)");
        QVERIFY2(src.contains(QStringLiteral("!QFile::rename(tmpPath, modelPath)")),
                 "downloadToModelsDir must check QFile::rename's return value — a failed move "
                 "must surface as failure, not a silently orphaned .part (item 1d)");
    }

    // Item 5 / design-review #4 (living doc): every job phase a Task announces
    // via phase() must have a row in the ONE phase registry
    // (JobListFormatter's phaseInfoFor() table, surfaced through
    // phaseLabel/phaseOverlayLabel — forwarded by the controller) — a phase with no row renders blank in the
    // bottom bar. This caught two real gaps historically ("labeling",
    // "benchmarking"). The label used to live in a BottomBar.qml switch (plus
    // a second, drifting switch in MainPanel); both are gone, so the guard now
    // scans src/core/tasks for the emitted phase vocabulary and checks the C++
    // registry. It reads BOTH the direct `phase(QStringLiteral("X"))` sites AND
    // DiarizeTask's indirect sub-phase literals (it emits `phase(subPhase)`).
    // ChaptersTask is SelfCumulative, so the fraction it emits IS the progress
    // bar — nothing downstream smooths it. It used to emit one value per LLM
    // window plus one for everything after, which on a long file meant ~20%
    // jumps and a long dead stretch while the merge pass (often MORE LLM calls
    // than generation) ran invisibly inside the final unit. Both long loops
    // live in LabelingEngine, so the only way the bar can move during them is
    // the progress callbacks. There's no unit-testable harness for the LLM
    // path (it needs a real GGUF), hence a source scan.
    void chaptersTaskReportsProgressInsideBothLlmPasses()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_SRC_DIR "/core/tasks/ChaptersTask.cpp")));
        QVERIFY2(!src.isEmpty(), "src/core/tasks/ChaptersTask.cpp must exist");
        const QString body = functionBody(src, QStringLiteral("void ChaptersTask::run("));
        QVERIFY2(!body.isEmpty(), "ChaptersTask::run() not found — scan anchor drifted");

        // A callback argument after the errorOut pointer is what makes the bar
        // move DURING each pass rather than only between calls.
        QVERIFY2(body.contains(QStringLiteral("generateChapters(")) && body.contains(QStringLiteral("&genError, [")),
                 "ChaptersTask::run() must pass a progress callback into generateChapters() — without it "
                 "the bar freezes for the whole of each generation window");
        QVERIFY2(body.contains(QStringLiteral("mergeSimilarChapters(")) && body.contains(QStringLiteral("&mergeError, [")),
                 "ChaptersTask::run() must pass a progress callback into mergeSimilarChapters() — without it "
                 "the merge pass is a long dead stretch at the end of the bar");

        // The old coarse denominator must be gone, not merely supplemented.
        QVERIFY2(!body.contains(QStringLiteral("progressUnits")),
                 "the one-unit-per-window progressUnits denominator is back — that's the 20%-jump bug");

        // The merge pass is minutes of work on a long file; it must say so
        // rather than leaving "chaptering…" up as if the first pass hung.
        QVERIFY2(body.contains(QStringLiteral("phase(QStringLiteral(\"merging-chapters\"))")),
                 "the merge pass must emit its own phase slug");
    }

    // RelabelTask is SelfCumulative (its emitted fraction IS the bar) and its
    // one label() call is the only long stretch — without a progress callback
    // the bar sits at 0 for the whole pass and snaps to 100% at the end (the
    // "labeling stays at 0% then jumps to 100" report). The LLM path needs a
    // real GGUF, so this is a source scan, same as the ChaptersTask guard above.
    void relabelTaskReportsProgressInsideItsLlmPass()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_SRC_DIR "/core/tasks/RelabelTask.cpp")));
        QVERIFY2(!src.isEmpty(), "src/core/tasks/RelabelTask.cpp must exist");
        const QString body = functionBody(src, QStringLiteral("void RelabelTask::run("));
        QVERIFY2(!body.isEmpty(), "RelabelTask::run() not found — scan anchor drifted");
        QVERIFY2(body.contains(QStringLiteral("label(")) && body.contains(QStringLiteral("&genError, [")),
                 "RelabelTask::run() must pass a progress callback into label() — without it the labeling "
                 "bar freezes at 0% and jumps to 100% when the single LLM call returns");
    }

    // The cancel→partial wiring in TranscribeTask::run() can't be unit-tested
    // without a real downloaded model, so guard both halves of the marker
    // lifecycle by source scan: the `partial: true` frontmatter is written up
    // front and cleared ONLY on the completed path. Dropping either half would
    // reintroduce the "a cancelled .md looks done" bug.
    void transcribeTask_writesPartialMarkerUpFrontAndClearsOnCompletion()
    {
        const QString src = stripComments(readAll(
            QStringLiteral(SUOP_SRC_DIR "/core/tasks/TranscribeTask.cpp")));
        QVERIFY2(!src.isEmpty(), "src/core/tasks/TranscribeTask.cpp must exist");
        // (a) marker written up front with the frontmatter block
        QVERIFY2(src.contains(QStringLiteral("/*partial=*/true")),
                 "TranscribeTask must write writeFrontmatter(..., /*partial=*/true)");
        // (b) marker cleared on the completed path
        QVERIFY2(src.contains(QStringLiteral("QStringLiteral(\"partial\")")),
                 "TranscribeTask must remove the partial frontmatter key on completion");
        QVERIFY2(src.contains(QStringLiteral("removeIfEmpty=*/true")),
                 "the partial key must be removed via upsertKey(..., removeIfEmpty)");
    }

    void phaseRegistryCoversEveryEmittedPhase()
    {
        QSet<QString> emitted;
        const QRegularExpression directEmit(QStringLiteral("phase\\(QStringLiteral\\(\"([^\"]+)\"\\)\\)"));
        // DiarizeTask emits `phase(subPhase)` where subPhase is one of these
        // two literals — the direct regex can't see the indirection, so pick
        // the "detecting-speakers-*" sub-phase literals up explicitly.
        const QRegularExpression subPhaseLiteral(
            QStringLiteral("QStringLiteral\\(\"(detecting-speakers-[a-z]+)\"\\)"));
        QDirIterator it(QStringLiteral(SUOP_SRC_DIR "/core/tasks"), {QStringLiteral("*.cpp")}, QDir::Files);
        while (it.hasNext()) {
            const QString src = readAll(it.next());
            for (const QRegularExpression &re : {directEmit, subPhaseLiteral}) {
                auto m = re.globalMatch(src);
                while (m.hasNext())
                    emitted.insert(m.next().captured(1));
            }
        }
        QVERIFY2(emitted.size() >= 12, "found too few emitted phases — scan regex/path drifted");
        QVERIFY2(emitted.contains(QStringLiteral("detecting-speakers-segmenting")),
                 "sub-phase scan drifted — DiarizeTask's finer detecting-speakers phases not seen");

        // The registry lives in JobListFormatter (not the controller, and not
        // BottomBar/MainPanel — it moved out of TranscriptionController.cpp on
        // 2026-07-20), so read it there. A row is
        // `{QStringLiteral("slug"), JobListFormatter::tr(`.
        const QString registry = readAll(QStringLiteral(SUOP_SRC_DIR "/core/JobListFormatter.cpp"));
        QVERIFY2(!registry.isEmpty(),
                 "src/core/JobListFormatter.cpp must exist — the phase registry moved there, and an "
                 "unreadable path would silently stop this guard from checking anything");
        for (const QString &phase : emitted) {
            const QString row = QStringLiteral("{QStringLiteral(\"%1\"), JobListFormatter::tr(").arg(phase);
            QVERIFY2(registry.contains(row),
                     qPrintable(QStringLiteral("phase \"%1\" is emitted by a Task but has no row in "
                                               "JobListFormatter's phase registry — phaseLabel() "
                                               "would return blank (design-review #4)")
                                    .arg(phase)));
        }
    }

    // Design-review #4 corollary: the two per-phase switches the registry
    // replaced must stay gone. A reintroduced `phaseLabel(` switch in
    // BottomBar or `case "transcribing"`-style phase switch in MainPanel's
    // overlay label would reopen the drift the registry closed.
    void phaseLabelSwitchesStayRetired()
    {
        const QString bottom = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml")));
        QVERIFY2(!bottom.contains(QStringLiteral("function phaseLabel(")),
                 "BottomBar.qml reintroduced a phaseLabel() switch — the phase→label map is the "
                 "controller's phaseLabel(), call transcriptionController.phaseLabel() instead");
        const QString main = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        const QString overlay = functionBody(main, QStringLiteral("readonly property string transcribingOverlayLabel"));
        QVERIFY2(!overlay.isEmpty(), "could not locate MainPanel.transcribingOverlayLabel");
        QVERIFY2(!overlay.contains(QStringLiteral("case \"")),
                 "MainPanel.transcribingOverlayLabel reintroduced a phase switch — use the controller's "
                 "phaseOverlayLabel() (the overlay column of the one phase registry) instead");
    }

    // Bug (live test #3): a restored file must become a TRUE selection, not just
    // the currentPath outline, or it can't anchor a Shift-click. The restore's
    // rebuild() fires modelReset (which clears the selection), so the Sidebar
    // has to re-establish it from the model's restore signals. This guards that
    // all three restore signals are handled and route through selectRestoredNote
    // (the behavior itself is proven in tst_sidebar_selectall's
    // restoredNote_becomesShiftClickAnchor). Without the wiring, the tree
    // "selection" after an undo is a dead outline again.
    void sidebar_restoreReestablishesSelectionAnchor()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        for (const QString &sig : {QStringLiteral("onFileRestored"), QStringLiteral("onTranscriptRestored"),
                                   QStringLiteral("onAudioRestoredForTranscript")}) {
            QVERIFY2(src.contains(sig),
                     qPrintable(QStringLiteral("Sidebar must handle the model's %1 restore signal to "
                                               "re-select the restored file").arg(sig)));
        }
        QVERIFY2(src.contains(QStringLiteral("function selectRestoredNote")),
                 "Sidebar must define selectRestoredNote() (the restore re-selection helper)");
        QVERIFY2(src.contains(QStringLiteral("selectRestoredNote(")),
                 "the restore signal handlers must call selectRestoredNote() so a restored file becomes a "
                 "real Shift-anchorable selection, not just the currentPath outline");
    }

    // Item 7 (drift fix, now enforced structurally): the three delete-confirm
    // dialogs must be the ONE shared ConfirmPopup, not hand-kept siblings. They
    // used to be three copies of the same Popup chrome that drifted apart (one
    // page's dialog set only `modal`, so Escape / click-outside / backdrop-dim
    // behaved differently depending on which page you were on). ConfirmPopup
    // bakes in modal + dim + focus + closePolicy, so "they behave the same" is
    // now guaranteed by construction — this guards that each call site actually
    // uses it and that the component still carries the shared behavior.
    void modelsPage_deleteConfirmMatchesSiblingDialogs()
    {
        // Each of the three delete dialogs is a ConfirmPopup (not a bespoke Popup).
        struct Site { const char *file; const char *id; };
        const Site sites[] = {
            {SUOP_QML_DIR "/ModelsPage.qml", "id: deleteConfirmPopup"},
            {SUOP_QML_DIR "/Sidebar.qml", "id: fileDeleteConfirmPopup"},
            {SUOP_QML_DIR "/MainPanel.qml", "id: deleteFileConfirmPopup"},
        };
        for (const Site &s : sites) {
            const QString src = stripComments(readAll(QString::fromLatin1(s.file)));
            const int anchor = src.indexOf(QString::fromLatin1(s.id));
            QVERIFY2(anchor >= 0, qPrintable(QStringLiteral("could not find %1").arg(s.id)));
            // The type name sits just before the id line, within the couple
            // hundred chars preceding it — scope the search to that window so a
            // later "ConfirmPopup" elsewhere in the file can't satisfy it.
            const int windowStart = qMax(0, anchor - 200);
            const QString before = src.mid(windowStart, anchor - windowStart);
            QVERIFY2(before.contains(QStringLiteral("ConfirmPopup {")),
                     qPrintable(QStringLiteral("%1 must be a ConfirmPopup so its "
                                               "delete UX matches the other pages").arg(s.id)));
        }
        // ...and ConfirmPopup itself carries the shared modal/dim/focus/closePolicy
        // behavior the three used to each spell out (and drift on).
        const QString comp = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/ConfirmPopup.qml")));
        QVERIFY2(comp.contains(QStringLiteral("closePolicy")),
                 "ConfirmPopup must set closePolicy (Escape/click-outside cancels)");
        QVERIFY2(comp.contains(QStringLiteral("dim: true")),
                 "ConfirmPopup must dim the backdrop");
        QVERIFY2(comp.contains(QStringLiteral("focus: true")),
                 "ConfirmPopup must take focus (so Escape works)");
        QVERIFY2(comp.contains(QStringLiteral("modal: true")),
                 "ConfirmPopup must be modal");
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
        const QString src = stripComments(sidebarTreeSrc());
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("function moveNoteSelection(direction)")),
                 "Sidebar.qml must define moveNoteSelection() to back Up/Down navigation");

        // The focus requirement now lives in ONE shared gate, noteShortcutsEnabled
        // (it grew a second conjunct — no inline rename in flight — so every
        // note shortcut had to stop hand-rolling the focus test; see the Delete
        // key guard). The gate's own definition must still carry the focus test,
        // NOT sidebar.currentPath (MainPanel's open-file state, unrelated to
        // keyboard focus).
        const int gateIdx = src.indexOf(QStringLiteral("readonly property bool noteShortcutsEnabled"));
        QVERIFY2(gateIdx >= 0, "Sidebar must define a shared noteShortcutsEnabled gate");
        QVERIFY2(src.mid(gateIdx, 220).contains(QStringLiteral("tree.activeFocus || searchResultsView.activeFocus")),
                 "noteShortcutsEnabled must gate on the tree/search-results view actually having focus, "
                 "not on sidebar.currentPath (MainPanel's open-file state, unrelated to keyboard focus)");

        const int upIdx = src.indexOf(QStringLiteral("sequence: \"Up\""));
        const int downIdx = src.indexOf(QStringLiteral("sequence: \"Down\""));
        QVERIFY2(upIdx >= 0, "Up must be wired as a Shortcut");
        QVERIFY2(downIdx >= 0, "Down must be wired as a Shortcut");
        const QString upBlock = src.mid(upIdx, 200);
        const QString downBlock = src.mid(downIdx, 200);
        QVERIFY2(upBlock.contains(QStringLiteral("sidebar.noteShortcutsEnabled")),
                 "Up shortcut must be gated on the shared noteShortcutsEnabled");
        QVERIFY2(downBlock.contains(QStringLiteral("sidebar.noteShortcutsEnabled")),
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
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml")));
        QVERIFY2(!src.isEmpty(), "qml/SettingsPopup.qml must exist");

        // (a) The tier call sites are now SliderSetting rows; each still passes a
        // non-zero stepSize (0.5) so the row snaps to the 3-tier low/mid/high
        // phrasing — no finer granularity actually exists.
        for (const QString &sliderId : {QStringLiteral("tagReuseBiasSlider"), QStringLiteral("wordinessSlider")}) {
            const QString block = enclosingBlock(src, QStringLiteral("id: %1").arg(sliderId));
            QVERIFY2(!block.isEmpty(), qPrintable(sliderId + QStringLiteral(" must exist")));
            QVERIFY2(block.contains(QStringLiteral("stepSize: 0.5")),
                     qPrintable(sliderId
                                + QStringLiteral(" must pass stepSize: 0.5 to match the 3-tier prompt phrasing "
                                                  "(low/mid/high) — no finer granularity actually exists")));
        }

        // (b) The snap itself is derived, not spelled out per row: SliderSetting
        // turns a non-zero stepSize into Slider.SnapAlways so dragging lands on a
        // tier instead of an intermediate value. If that derivation regresses to
        // NoSnap, the call sites above look correct but no row would actually snap.
        const QString row = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/SliderSetting.qml")));
        QVERIFY2(!row.isEmpty(), "qml/SliderSetting.qml must exist");
        const QRegularExpression snap(
            QStringLiteral("snapMode:\\s*root\\.stepSize\\s*>\\s*0\\s*\\?\\s*Slider\\.SnapAlways"));
        QVERIFY2(snap.match(row).hasMatch(),
                 "SliderSetting.qml must derive snapMode: Slider.SnapAlways from a non-zero stepSize "
                 "(root.stepSize > 0 ? Slider.SnapAlways : ...) so the tier rows above snap to low/mid/high");
    }

    // Queue item 044: every settings row's leading name Label shares one
    // fixed-width column. Long names ("Left content margin", "Tag reuse bias")
    // must WRAP there rather than elide — a single-line elided label lets the
    // slider truncate the name. Source-scan: QML layout inside a Popup, no
    // QQuickView harness.
    //
    // The bulk of these rows now live in the SliderSetting / ToggleSetting
    // components, so the wrapping idiom is asserted THERE (once) plus on the
    // handful of rows still spelled out inline in SettingsPopup.qml.
    void settingsPopup_rowNameLabelsWrapInsteadOfEliding()
    {
        const QRegularExpression wrapped(
            QStringLiteral("Layout\\.preferredWidth:\\s*90\\s*\\n\\s*Layout\\.alignment:\\s*Qt\\.AlignVCenter"
                           "\\s*\\n\\s*wrapMode:\\s*Text\\.WordWrap"));
        const QRegularExpression elided(
            QStringLiteral("Layout\\.preferredWidth:\\s*90\\s*\\n\\s*elide:"));

        // The shared name column: SliderSetting's leading Label owns the 90px
        // wrapping column every numeric row inherits. If this regresses to an
        // elide, every slider row's name would truncate at once.
        const QString slider = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/SliderSetting.qml")));
        QVERIFY2(!slider.isEmpty(), "qml/SliderSetting.qml must exist");
        QVERIFY2(wrapped.match(slider).hasMatch(),
                 "SliderSetting.qml's name Label must use the wrapping name-column idiom "
                 "(Layout.preferredWidth: 90 + Layout.alignment: Qt.AlignVCenter + wrapMode: Text.WordWrap) — "
                 "an elide here would truncate every slider row's name behind its slider");
        QVERIFY2(!elided.match(slider).hasMatch(),
                 "SliderSetting.qml's name Label must not be single-line elided");

        // ToggleSetting keeps wrapping opt-in (labelWrap, default off) because
        // only the two long-caption switch rows ever needed it — but the knob and
        // its WordWrap branch must still exist so those rows can wrap.
        const QString toggle = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/ToggleSetting.qml")));
        QVERIFY2(!toggle.isEmpty(), "qml/ToggleSetting.qml must exist");
        QVERIFY2(toggle.contains(QStringLiteral("property bool labelWrap")),
                 "ToggleSetting.qml must expose a labelWrap knob so a long switch caption can wrap");
        const QRegularExpression toggleWrap(
            QStringLiteral("wrapMode:\\s*root\\.labelWrap\\s*\\?\\s*Text\\.WordWrap"));
        QVERIFY2(toggleWrap.match(toggle).hasMatch(),
                 "ToggleSetting.qml's caption Label must wrap when labelWrap is set "
                 "(wrapMode: root.labelWrap ? Text.WordWrap : ...)");

        // The long-caption switch rows must actually turn wrapping on — otherwise
        // labelWrap exists but nothing uses it and the captions run under the Switch.
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml")));
        QVERIFY2(!src.isEmpty(), "qml/SettingsPopup.qml must exist");
        QVERIFY2(src.count(QStringLiteral("labelWrap: true")) >= 2,
                 "both long-caption ToggleSetting rows (\"Delete original parts…\", \"Combine "
                 "automatically…\") must pass labelWrap: true");

        // Rows still authored inline in SettingsPopup.qml (icon-pack, the two
        // diarization-model pickers, chapter density) must keep wrapping too, and
        // none may elide.
        QVERIFY2(!elided.match(src).hasMatch(),
                 "a settings row name Label must not be single-line elided — long names "
                 "like \"Left content margin\" get truncated by the slider beside them; "
                 "use Layout.alignment: Qt.AlignVCenter + wrapMode: Text.WordWrap instead");
        int wrappedRows = 0;
        auto it = wrapped.globalMatch(src);
        while (it.hasNext()) {
            it.next();
            ++wrappedRows;
        }
        QVERIFY2(wrappedRows >= 4,
                 qPrintable(QStringLiteral("every inline settings row name Label (4 at the time of writing) "
                                           "must use the wrapping name-column idiom; found %1")
                                .arg(wrappedRows)));
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
    // bulk-ribbon buttons' "always narrow to the current selection" (never
    // "everything eligible") behavior all still exist.
    void sidebar_multiSelectShiftCtrlClick()
    {
        const QString src = sidebarTreeSrc();
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

        // Bulk ribbon buttons always narrow to the current selection
        // (intersected with eligibility) — an empty selection means an empty
        // target set, not "everything eligible". Item 041: eligibility is
        // derived from the MODEL (searchResultNotes()) via
        // refreshSelectionEligibility(), never pushed by realized TreeView
        // delegates (reuseItems recycling silently dropped the registration).
        QVERIFY2(src.contains(QStringLiteral("function refreshSelectionEligibility()")),
                 "must define refreshSelectionEligibility() to narrow model eligibility to the selection");
        const QString eligBody = functionBody(src, QStringLiteral("function doRefreshSelectionEligibility()"));
        QVERIFY2(eligBody.contains(QStringLiteral("selected.indexOf(p) >= 0")),
                 "doRefreshSelectionEligibility must intersect eligibility with the current selection");
        QVERIFY2(!eligBody.contains(QStringLiteral("selectedPaths.length < 2")),
                 "the ribbon must not fall back to \"everything eligible\" for 0/1 selected — "
                 "selectAllButton/Ctrl+A cover that case explicitly");
        QVERIFY2(src.contains(QStringLiteral("sidebar.selectionTranscribePaths")),
                 "transcribeAllButton must narrow to the selection via selectionTranscribePaths");
        QVERIFY2(src.contains(QStringLiteral("sidebar.selectionRelabelPaths")),
                 "relabelAllButton must narrow to the selection via selectionRelabelPaths");
        QVERIFY2(src.contains(QStringLiteral("sidebar.selectionChaptersPaths")),
                 "chaptersAllButton must narrow to the selection via selectionChaptersPaths");
        // Root-cause guard (item 041): ribbon eligibility must NEVER be pushed
        // by realized TreeView delegates — reuseItems recycling silently drops
        // that registration. The setBulk/Relabel/Chapters setters are gone.
        QVERIFY2(!src.contains(QStringLiteral("setBulkEligible(")),
                 "ribbon eligibility must be model-derived, not pushed by delegates (item 041)");
        QVERIFY2(!src.contains(QStringLiteral("setRelabelEligible(")),
                 "ribbon eligibility must be model-derived, not pushed by delegates (item 041)");
        QVERIFY2(!src.contains(QStringLiteral("setChaptersEligible(")),
                 "ribbon eligibility must be model-derived, not pushed by delegates (item 041)");

        // Ribbon's prepended select-all button + divider, and its Ctrl+A
        // shortcut mirror.
        QVERIFY2(src.contains(QStringLiteral("function selectAllVisible()")),
                 "must define a selectAllVisible() helper shared by the ribbon button and Ctrl+A");
        const int selectAllBtnIdx = src.indexOf(QStringLiteral("id: selectAllButton"));
        QVERIFY2(selectAllBtnIdx >= 0, "ribbon must have a prepended selectAllButton");
        const int dividerIdx = src.indexOf(QStringLiteral("id: ribbonDivider"));
        QVERIFY2(dividerIdx >= 0, "ribbon must have a divider between selectAllButton and the "
                                   "bulk action buttons");
        QVERIFY2(dividerIdx > selectAllBtnIdx && dividerIdx < src.indexOf(QStringLiteral("id: transcribeAllButton")),
                 "the divider must sit between selectAllButton and transcribeAllButton");

        // Combine is a different KIND of ribbon action from the bulk pipeline
        // to its left — it scans allAudioPaths() vault-wide rather than
        // narrowing to the selection — so a divider separates the two.
        const int runAllIdx = src.indexOf(QStringLiteral("id: runAllButton"));
        const int combineIdx = src.indexOf(QStringLiteral("id: combineButton"));
        const int combineDividerIdx = src.indexOf(QStringLiteral("id: ribbonCombineDivider"));
        QVERIFY(runAllIdx >= 0 && combineIdx >= 0);
        QVERIFY2(combineDividerIdx >= 0, "ribbon must have a divider between runAllButton and combineButton");
        QVERIFY2(combineDividerIdx > runAllIdx && combineDividerIdx < combineIdx,
                 "the combine divider must sit between runAllButton and combineButton");
        QVERIFY2(src.contains(QStringLiteral("sequence: \"Ctrl+A\"")),
                 "must bind a Ctrl+A Shortcut");
        const int ctrlAIdx = src.indexOf(QStringLiteral("sequence: \"Ctrl+A\""));
        QVERIFY(ctrlAIdx >= 0);
        const QString ctrlABlock = src.mid(ctrlAIdx, 200);
        QVERIFY2(ctrlABlock.contains(QStringLiteral("sidebar.selectAllVisible()")),
                 "Ctrl+A must call selectAllVisible()");

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

    // The ribbon WRAPS. It outgrew a single row once the destructive trio
    // landed (CLAUDE.md action parity), and a sidebar is narrow and
    // user-resizable, so a fixed row silently squeezed or clipped buttons.
    //
    // The load-bearing half is that a Flow ignores Layout attached properties
    // ENTIRELY — it positions by plain width/height. A child left on
    // Layout.preferredWidth inside it doesn't warn; it collapses to an
    // implicit size and the button becomes an invisible 0-width hit target.
    // So this guards the conversion, not just the Flow's existence.
    void sidebar_ribbonWraps_andItsChildrenSizeWithoutLayoutAttachments()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        // The button flow was extracted to SidebarRibbon.qml, whose ROOT is the
        // Flow — so the type/id check reads that file directly. (Sidebar.qml
        // still has `id: ribbonFlow` on the SidebarRibbon instance, which is
        // what the ribbon height binding below refers to.)
        const QString flowSrc = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/SidebarRibbon.qml")));
        QVERIFY2(!flowSrc.isEmpty(), "qml/SidebarRibbon.qml must exist");
        const int ribbonFlowIdx = flowSrc.indexOf(QStringLiteral("id: ribbonFlow"));
        QVERIFY2(ribbonFlowIdx >= 0, "the ribbon's button container must be `Flow { id: ribbonFlow }` "
                                      "so the buttons wrap at a narrow sidebar width");
        // The element type sits just before the id: line. A small backward
        // window (not an exact-whitespace match) so reindentation can't break
        // the guard — this file's own doc comments warn against fixed windows
        // that rot silently, but a "does the token appear right here" check is
        // exactly what a small window is for.
        QVERIFY2(flowSrc.mid(qMax(0, ribbonFlowIdx - 40), 40).contains(QStringLiteral("Flow {")),
                 "ribbonFlow must be a Flow — a RowLayout does not wrap");
        const QString ribbonFlow = enclosingBlock(flowSrc, QStringLiteral("id: ribbonFlow"));
        QVERIFY(!ribbonFlow.isEmpty());

        // No Layout.* attachment may survive inside it. Layout.preferredWidth/
        // Height are the ones that were converted; fillWidth/alignment are the
        // ones a Flow silently gives nothing to fill and nothing to align.
        for (const QString &attachment : { QStringLiteral("Layout.preferredWidth"),
                                            QStringLiteral("Layout.preferredHeight"),
                                            QStringLiteral("Layout.fillWidth"),
                                            QStringLiteral("Layout.alignment") }) {
            QVERIFY2(!ribbonFlow.contains(attachment),
                     qPrintable(QStringLiteral("%1 inside ribbonFlow is a silent no-op — a Flow positions "
                                                "by plain width/height. Use width:/height: instead.")
                                    .arg(attachment)));
        }

        // The ribbon's own height must track the Flow's wrapped height. It was
        // a literal 32 (one 22px row + padding), which is exactly the kind of
        // magic number this file's ribbon height binding already got burned by
        // once — see its doc comment in Sidebar.qml.
        const QString ribbon = enclosingBlock(src, QStringLiteral("id: ribbon"));
        QVERIFY2(ribbon.contains(QStringLiteral("ribbonFlow.implicitHeight")),
                 "the ribbon's collapsed height must follow ribbonFlow.implicitHeight, not a fixed "
                 "one-row constant — it wraps now, so the row count is not knowable up front");
    }

    // The Delete key deletes the sidebar's selection, mirroring the ribbon's
    // deleteFileAllButton.
    //
    // The renamingPath gate is the load-bearing half and the reason this guard
    // exists at all. The other note shortcuts reason that "a TextField
    // elsewhere has focus, so the tree does not" — which holds for the search
    // fields (they sit outside the tree) but NOT for the inline rename editor,
    // which is a TextField INSIDE a tree delegate. TreeView is a FocusScope, so
    // tree.activeFocus is TRUE while you type in that editor (Sidebar.qml's own
    // treeRowTap comment documents exactly this: forceActiveFocus() on the
    // TreeView hands focus straight back to its focused child). Without the
    // gate, pressing Delete to erase a character mid-rename trashes the file
    // being renamed.
    void sidebar_deleteKey_deletesSelection_andIsInertDuringAnInlineRename()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("sequence: StandardKey.Delete")),
                 "Sidebar must bind a Delete Shortcut so the file tree's selection can be deleted "
                 "from the keyboard");
        const QString deleteBlock = enclosingBlock(src, QStringLiteral("sequence: StandardKey.Delete"));
        QVERIFY2(deleteBlock.contains(QStringLiteral("sidebar.beginDeleteFile(")),
                 "the Delete key must route through beginDeleteFile() — the trash-first path, so it is "
                 "undoable from the snackbar, not deleteFilesPermanently()");
        // Item 055: Delete uses the selection-following deleteShortcutEnabled
        // gate (NOT noteShortcutsEnabled), so it fires even after focus leaves
        // the tree while the selection stays highlighted.
        QVERIFY2(deleteBlock.contains(QStringLiteral("sidebar.deleteShortcutEnabled")),
                 "the Delete key must use the deleteShortcutEnabled gate (item 055)");

        // The Delete gate: a selection, not editing text, no rename in flight —
        // and deliberately NOT tree.activeFocus (the selection persists after
        // focus leaves the tree).
        const int delGateIdx = src.indexOf(QStringLiteral("readonly property bool deleteShortcutEnabled"));
        QVERIFY2(delGateIdx >= 0, "Sidebar must define the deleteShortcutEnabled gate (item 055)");
        const QString delGate = src.mid(delGateIdx, 220);
        QVERIFY2(delGate.contains(QStringLiteral("sidebar.selectedPaths.length > 0")),
                 "deleteShortcutEnabled must be inert with an empty selection");
        QVERIFY2(delGate.contains(QStringLiteral("!sidebar.textFieldHasFocus")),
                 "deleteShortcutEnabled must be suppressed while a text field is being edited — "
                 "otherwise Delete mid-edit (rename/transcript/title) trashes the selection");
        QVERIFY2(!delGate.contains(QStringLiteral("tree.activeFocus")),
                 "deleteShortcutEnabled must NOT require tree focus — the selection stays highlighted "
                 "after focus drifts to the main panel (item 055)");

        // textFieldHasFocus is the duck-typed text-editing test the Delete gate
        // relies on.
        const int tfIdx = src.indexOf(QStringLiteral("readonly property bool textFieldHasFocus"));
        QVERIFY2(tfIdx >= 0, "Sidebar must define textFieldHasFocus (item 055)");
        const QString tf = src.mid(tfIdx, 220);
        QVERIFY2(tf.contains(QStringLiteral("Window.activeFocusItem"))
                     && tf.contains(QStringLiteral("cursorPosition")),
                 "textFieldHasFocus must duck-type the window's active focus item on cursorPosition");

        // The navigation gate is unchanged: focus AND no rename in flight.
        const int gateIdx = src.indexOf(QStringLiteral("readonly property bool noteShortcutsEnabled"));
        QVERIFY2(gateIdx >= 0, "Sidebar must define a shared noteShortcutsEnabled gate for the note "
                                "shortcuts");
        const QString gate = src.mid(gateIdx, 220);
        QVERIFY2(gate.contains(QStringLiteral("sidebar.renamingPath === \"\"")),
                 "noteShortcutsEnabled MUST require renamingPath === \"\": TreeView is a FocusScope, so "
                 "tree.activeFocus stays true while the inline rename editor (a TextField INSIDE a tree "
                 "delegate) has focus");
        QVERIFY2(gate.contains(QStringLiteral("tree.activeFocus"))
                     && gate.contains(QStringLiteral("searchResultsView.activeFocus")),
                 "noteShortcutsEnabled must still require the tree/search-results view to have focus");
    }

    // Combine placeholders: pressing Combine must change the tree NOW, not when
    // ffmpeg finishes. See VaultTreeModel::setPendingCombines()'s doc comment.
    void sidebar_pendingCombines_pushedFromTheQueue_andRenderedAsAnonymousProgress()
    {
        const QString src = stripComments(sidebarTreeSrc());
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        // Derived from the controller's own jobs list — a combine job already
        // carries its output path (as fileUrl) and its ordered sources, so
        // there is nothing for the controller to add for this.
        const QString pending = functionBody(src, QStringLiteral("function pendingCombines()"));
        QVERIFY2(!pending.isEmpty(), "Sidebar must define pendingCombines()");
        QVERIFY2(pending.contains(QStringLiteral("\"combine\"")),
                 "pendingCombines() must select combine-kind jobs");
        QVERIFY2(pending.contains(QStringLiteral("\"queued\"")) && pending.contains(QStringLiteral("\"active\"")),
                 "pendingCombines() must cover QUEUED as well as active jobs — a combine waiting behind "
                 "other work is exactly the case where the wait is longest and the feedback matters most");
        QVERIFY2(pending.contains(QStringLiteral("combineSourcePaths")),
                 "pendingCombines() must carry each job's source parts so the model can hide them");

        QVERIFY2(functionBody(src, QStringLiteral("function pushPendingCombines()"))
                     .contains(QStringLiteral("setPendingCombines(")),
                 "pushPendingCombines() must push into VaultTreeModel.setPendingCombines()");
        // The queue is the only thing that can change which combines are
        // pending — enqueue, start and finish all land on queueChanged.
        QVERIFY2(src.contains(QStringLiteral("function onQueueChanged() { sidebar.pushPendingCombines() }")),
                 "Sidebar must re-push pending combines on the controller's queueChanged");

        // The row is anonymous progress, never a filename: the merged file does
        // not exist yet, so there is nothing honest to call it.
        const QString placeholderRow = enclosingBlock(src, QStringLiteral("id: combinePlaceholderRow"));
        QVERIFY2(!placeholderRow.isEmpty(), "the tree delegate must render a combinePlaceholderRow");
        QVERIFY2(placeholderRow.contains(QStringLiteral("treeDelegate.isCombinePlaceholder")),
                 "combinePlaceholderRow must only show for a placeholder row");
        QVERIFY2(!placeholderRow.contains(QStringLiteral("treeDelegate.name")),
                 "the placeholder must NOT render a filename — no file exists yet to name");
        QVERIFY2(placeholderRow.contains(QStringLiteral("combineSourceCount")),
                 "the placeholder must say how many parts it is merging");

        // Not clickable: it has nothing to open, rename or act on.
        QVERIFY2(enclosingBlock(src, QStringLiteral("id: treeRowTap"))
                     .contains(QStringLiteral("enabled: !treeDelegate.isCombinePlaceholder")),
                 "the row TapHandler must be disabled for a combine placeholder — it has no file to open");
        QVERIFY2(enclosingBlock(src, QStringLiteral("id: treeRowContextTap"))
                     .contains(QStringLiteral("enabled: !treeDelegate.isCombinePlaceholder")),
                 "the context-menu TapHandler must be disabled for a combine placeholder — every entry "
                 "in that menu acts on a file");
    }

    // Combining is RULE-DRIVEN ONLY: a merge may only ever happen for a group
    // that AppSettings.combineRules actually matched. The sidebar's two combine
    // affordances are the ribbon scan (runCombineScan(), every "ready" group)
    // and the per-row "combine this group" button (combineGroup(), one matched
    // group) — both take the rules as an argument.
    //
    // A selection-driven combine used to exist (the multi-select "Combine"
    // button and a "Combine selected (N)" context row, via a controller
    // combineSelected()). It was removed deliberately: it concatenated whatever
    // audio the user happened to select, with no rule consulted, so it would
    // merge unrelated recordings no rule would ever group — and it hard-coded
    // deleteOriginals=false, silently ignoring the configured setting the
    // rule-driven paths honor. No QQuickView in this suite to click the menu,
    // so this is a source-scan guard that it doesn't come back.
    void sidebar_combineIsRuleDrivenOnly()
    {
        const QString src = sidebarTreeSrc();
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        QVERIFY2(!src.contains(QStringLiteral("combineSelected")),
                 "Sidebar.qml must not offer a selection-driven combine (combineSelected/"
                 "combineSelectedFiles/ctxCombineSelectedRow): combining is only valid for a group "
                 "matched by AppSettings.combineRules, never for an arbitrary multi-selection");
        QVERIFY2(!src.contains(QStringLiteral("selectedAudioPaths")),
                 "selectedAudioPaths() existed only to feed the removed selection-driven combine");

        // The two legitimate, rule-driven entry points must still be wired: both
        // pass AppSettings.combineRules, which is what makes them rule-driven.
        QVERIFY2(src.contains(QStringLiteral(".runCombineScan(")),
                 "the ribbon combine button must still call runCombineScan()");
        QVERIFY2(src.contains(QStringLiteral(".combineGroup(")),
                 "the per-row \"combine this group\" button must still call combineGroup()");
        for (const char *call : {".runCombineScan(", ".combineGroup("}) {
            const int idx = src.indexOf(QString::fromLatin1(call));
            QVERIFY(idx >= 0);
            QVERIFY2(src.mid(idx, 300).contains(QStringLiteral("AppSettings.combineRules")),
                     qPrintable(QStringLiteral("%1 must be passed AppSettings.combineRules — the rules "
                                               "are what decide which files may be merged")
                                    .arg(QString::fromLatin1(call))));
        }
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

    // StyleGuide.md "Snackbars": every transient notification goes through the
    // Notifications singleton, so they all land in the one bottom-right stack.
    // The four hand-rolled per-panel auto-clearing overlay banners this
    // replaced (importError/rowActionError/speakerOpError/transcriptEditError)
    // must not come back — re-adding one is how the app silently drifts back to
    // four notification surfaces. tst_snackbar.cpp does the runtime half; this
    // is the regression guard.
    void snackbar_transientFeedbackGoesThroughTheNotificationsSingleton()
    {
        const QStringList retired = {
            QStringLiteral("importError"),      QStringLiteral("rowActionError"),
            QStringLiteral("speakerOpError"),   QStringLiteral("transcriptEditError"),
        };
        const QStringList panels = { QStringLiteral("Main.qml"), QStringLiteral("Sidebar.qml"),
                                     QStringLiteral("MainPanel.qml") };
        for (const QString &panel : panels) {
            // stripComments throughout: these files' comments legitimately name
            // the retired properties when explaining what replaced them.
            const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/") + panel));
            QVERIFY2(!src.isEmpty(), qPrintable(panel + QStringLiteral(" must exist")));
            for (const QString &prop : retired) {
                QVERIFY2(!src.contains(QStringLiteral("property string ") + prop),
                         qPrintable(panel + QStringLiteral(" reintroduces the retired per-panel banner "
                                                           "property '") + prop
                                    + QStringLiteral("' — transient feedback posts to the Notifications "
                                                     "singleton now (StyleGuide.md \"Snackbars\")")));
            }
        }

        // ...and each panel actually posts.
        for (const QString &panel : panels) {
            const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/") + panel));
            QVERIFY2(src.contains(QStringLiteral("Notifications.")),
                     qPrintable(panel + QStringLiteral(" must post transient feedback via the "
                                                       "Notifications singleton")));
        }
    }

    // StyleGuide.md "Snackbars": exactly ONE SnackbarHost, in Main.qml. A
    // second host (or a Snackbar parented at a call site) is exactly the
    // scattered-notifications state the singleton exists to end.
    void snackbar_singleHostInMainQmlOnly()
    {
        const QDir qmlDir{ QStringLiteral(SUOP_QML_DIR) };
        const QStringList files = qmlDir.entryList({ QStringLiteral("*.qml") }, QDir::Files);
        QVERIFY(!files.isEmpty());
        for (const QString &f : files) {
            if (f == QStringLiteral("SnackbarHost.qml") || f == QStringLiteral("Snackbar.qml"))
                continue;
            const QString src = stripComments(readAll(qmlDir.filePath(f)));
            const bool instantiatesHost = src.contains(QStringLiteral("SnackbarHost {"));
            const bool instantiatesBar = src.contains(QStringLiteral("Snackbar {"));
            if (f == QStringLiteral("Main.qml")) {
                QVERIFY2(instantiatesHost, "Main.qml must instantiate the one SnackbarHost");
                QVERIFY2(!instantiatesBar, "Main.qml must not parent a Snackbar directly — post to "
                                           "the Notifications singleton instead");
                continue;
            }
            QVERIFY2(!instantiatesHost,
                     qPrintable(f + QStringLiteral(" instantiates a second SnackbarHost — there must "
                                                   "be exactly one, in Main.qml")));
            QVERIFY2(!instantiatesBar,
                     qPrintable(f + QStringLiteral(" instantiates a Snackbar directly — post to the "
                                                   "Notifications singleton instead")));
        }
    }

    // StyleGuide.md "Snackbars": action/dismiss controls are Item + TapHandler,
    // never a focusable QQC2 Button/MouseArea. This app commits TextFields on
    // blur, so a focus-stealing control here fires real writes on press — and a
    // failed write posts another snackbar, mutating the queue mid-click.
    // tst_snackbar.cpp asserts the focus behavior at runtime; this guards the
    // mechanism that produces it.
    void snackbar_controlsAreNonFocusableTapHandlers()
    {
        const QString raw = readAll(QStringLiteral(SUOP_QML_DIR "/Snackbar.qml"));
        QVERIFY2(!raw.isEmpty(), "qml/Snackbar.qml must exist");
        // stripComments() is load-bearing: this file's own comments EXPLAIN why
        // it avoids MouseArea/Button, so a raw scan matches the prose and fails
        // on a compliant file.
        const QString src = stripComments(raw);
        QVERIFY2(!src.contains(QStringLiteral("MouseArea")),
                 "Snackbar.qml must not use MouseArea (cursor-bleed + focus-steal); use "
                 "HoverHandler + TapHandler");
        QVERIFY2(!src.contains(QStringLiteral("Button {")),
                 "Snackbar.qml must not use a QQC2 Button — it takes focus on press and this app "
                 "commits fields on blur");
        QVERIFY2(src.contains(QStringLiteral("TapHandler")),
                 "Snackbar.qml's action/dismiss controls must be TapHandler-driven");
        QVERIFY2(raw.contains(QStringLiteral("pragma ComponentBehavior: Bound")),
                 "Snackbar.qml reaches the outer `snackbar` id from its action delegate — without "
                 "ComponentBehavior: Bound that's unqualified access and the AOT compiler can't "
                 "compile those bindings");
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

    // Item 4: the model picker's CLOSED-state box must round its corners with
    // AppSettings.borderRadius — QQC2's default ComboBox background is
    // square-cornered, so the picker needs its own background: Rectangle
    // override (the dropdown popup was already fixed separately). The three
    // MainPanel pickers (transcription model + diarization segmentation /
    // embedding) share one reusable qml/ModelComboBox.qml, so this scans the
    // component. No QQuickView here to inspect the rendered corners, so this
    // is a source-scan guard that the background override exists.
    void mainPanel_modelComboClosedBoxIsRounded()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/ModelComboBox.qml"));
        QVERIFY2(!src.isEmpty(), "qml/ModelComboBox.qml must exist");

        // Scan only up to the popup override so this asserts the closed-box
        // background specifically, not the already-fixed popup background.
        const int popupIdx = src.indexOf(QStringLiteral("popup: Popup"));
        QVERIFY2(popupIdx >= 0, "ModelComboBox's popup override must exist");
        const QString closedBoxBlock = src.left(popupIdx);
        QVERIFY2(closedBoxBlock.contains(QStringLiteral("background: Rectangle")),
                 "ModelComboBox must override its closed-state background with a Rectangle (QQC2's default "
                 "box is square-cornered) — item 4");
        const int bgIdx = closedBoxBlock.indexOf(QStringLiteral("background: Rectangle"));
        const QString bgBlock = closedBoxBlock.mid(bgIdx, 200);
        QVERIFY2(bgBlock.contains(QStringLiteral("radius: AppSettings.borderRadius")),
                 "ModelComboBox's closed-state background must round its corners with "
                 "AppSettings.borderRadius (StyleGuide.md border-radius convention)");
        // The closed box must elide its current-selection label (a
        // width-capped box in a shared row otherwise silently clips the tail).
        QVERIFY2(closedBoxBlock.contains(QStringLiteral("elide: Text.ElideRight")),
                 "ModelComboBox's closed-state contentItem must elide (truncate) the selected label");
    }

    // The MainPanel note-page controls surface, next to the transcription
    // model, both diarization model pickers (segmentation + embedding) and the
    // diarization sensitivity slider — all bound to the same AppSettings keys
    // the "Detect speakers" / runAllJobs paths read. No QQuickView harness for
    // MainPanel (too many C++ deps), so this is a source-scan guard that the
    // three controls are mounted and wired to their settings.
    void mainPanel_hasDiarizationModelAndSensitivityControls()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("id: segmentationCombo")),
                 "MainPanel must mount a segmentation-model picker beside the transcription model");
        QVERIFY2(src.contains(QStringLiteral("AppSettings.diarizationSegmentationModel = id")),
                 "the segmentation picker must persist its choice to AppSettings.diarizationSegmentationModel");
        QVERIFY2(src.contains(QStringLiteral("id: embeddingCombo")),
                 "MainPanel must mount an embedding-model picker beside the transcription model");
        QVERIFY2(src.contains(QStringLiteral("AppSettings.diarizationEmbeddingModel = id")),
                 "the embedding picker must persist its choice to AppSettings.diarizationEmbeddingModel");
        QVERIFY2(src.contains(QStringLiteral("id: diarizationSensitivitySlider")),
                 "MainPanel must mount a diarization sensitivity slider");
        QVERIFY2(src.contains(QStringLiteral("AppSettings.diarizationSensitivity = value")),
                 "the sensitivity slider must persist to AppSettings.diarizationSensitivity");
    }

    // The Basic style's vertical ScrollBar is an overlay that floats flush
    // against the ScrollView's right edge and covers whatever content fills the
    // full width. Item 067 moved that geometry into ScrollPage: the note page's
    // scrolling content binds its width to notePage.availableContentWidth (=
    // inner ScrollView width - the shared scrollBarGutter), so the transcript
    // keeps its true 16px right inset and the bar floats in the reclaimed
    // gutter. ScrollPage owns the single gutter constant; the note page just
    // consumes it. Hand a raw availableWidth here and the transcript slides back
    // under the bar. No QQuickView harness for MainPanel (too many C++ deps), so
    // this is a source-scan guard.
    void mainPanel_transcriptScrollBarSitsInTheRightMarginGutter()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const QString content = enclosingBlock(src, QStringLiteral("id: scrollableContent"));
        QVERIFY2(!content.isEmpty(), "scrollableContent must exist");
        QVERIFY2(content.contains(QStringLiteral("width: notePage.availableContentWidth")),
                 "the transcript content must bind its width to notePage.availableContentWidth so the "
                 "overlay ScrollBar floats in ScrollPage's reclaimed gutter instead of on the transcript");

        // The shared base owns the gutter constant + the derived content width.
        const QString page = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/ScrollPage.qml")));
        QVERIFY2(page.contains(QStringLiteral("scrollBarGutter")),
                 "ScrollPage must own the single scrollBarGutter constant");
        QVERIFY2(page.contains(QStringLiteral("availableContentWidth")),
                 "ScrollPage must expose availableContentWidth (inner width minus the gutter)");
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

    // The kind BADGE label now lives on the Task primitive — each kind authors
    // its own Task::kindLabel() (the single source of truth that replaced
    // BottomBar's kindLabel() switch, which a new kind could forget). The bottom
    // bar must render that authored label (jobs()' "kindLabel" field /
    // currentJobKindLabel), never re-derive it from the slug. (The PHASE label
    // moved the same way — into the controller's phase registry; its coverage
    // is guarded by phaseRegistryCoversEveryEmittedPhase, no longer here.)
    void bottomBar_rendersTaskAuthoredKindLabel_notASwitch()
    {
        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");
        QVERIFY2(!bar.contains(QStringLiteral("function kindLabel")),
                 "BottomBar must NOT re-implement a kindLabel(kind) switch — the label is authored per-kind "
                 "on the Task (Task::kindLabel()); a switch here is the drift that mislabeled new kinds "
                 "\"Transcribe\"");
        QVERIFY2(bar.contains(QStringLiteral("modelData.kindLabel")),
                 "the Tasks-row badge must read the Task-authored jobs() \"kindLabel\" field");
        QVERIFY2(bar.contains(QStringLiteral("currentJobKindLabel")),
                 "the active-job badge must read the controller's currentJobKindLabel (the active task's "
                 "Task::kindLabel())");

        // Item 37: the import kind authors its own badge label.
        const QString importTask = readAll(QStringLiteral(SUOP_SRC_DIR "/core/tasks/ImportTask.h"));
        QVERIFY2(importTask.contains(QStringLiteral("kindLabel")),
                 "ImportTask must author its own kindLabel() (item 37) so import jobs get a readable badge");
    }

    // A USB grab is a normal queued job now (see DESIGN.md "USB import"), so it
    // renders through the same Tasks rows as every other kind. The badge label
    // is authored on the Task (UsbGrabTask::kindLabel()) — the whole point of
    // moving it off the QML switch is that a kind can't forget it and fall to a
    // default that LIES (the old default arm was "Transcribe", so an unlabelled
    // grab claimed to be a transcription of the destination folder). (Its
    // "grabbing" PHASE label lives in the controller's phase registry now,
    // guarded by phaseRegistryCoversEveryEmittedPhase.)
    void bottomBar_usbGrabKindHasBadge()
    {
        const QString grabTask = readAll(QStringLiteral(SUOP_SRC_DIR "/core/tasks/UsbGrabTask.h"));
        QVERIFY2(grabTask.contains(QStringLiteral("kindLabel")),
                 "UsbGrabTask must author its own kindLabel() so a grab isn't mislabelled");
    }

    // Item 52 (diarize kind): the standalone diarize job (kind "diarize") must
    // author its own kind badge so it isn't blank/mislabelled in the Tasks
    // popup. (Its phases — and chaptering/detecting-speech — are labelled via
    // the controller's phase registry, guarded by
    // phaseRegistryCoversEveryEmittedPhase.)
    void bottomBar_diarizeKindHasBadge()
    {
        const QString diarizeTask = readAll(QStringLiteral(SUOP_SRC_DIR "/core/tasks/DiarizeTask.h"));
        QVERIFY2(diarizeTask.contains(QStringLiteral("kindLabel")),
                 "DiarizeTask must author its own kindLabel() (item 52) so standalone detect-speakers "
                 "jobs get a readable kind badge in the Tasks popup");
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

    // Item 42, REVERSED by user decision: clicking the bottom-left current-file
    // path label opens the OS file manager again — revealing the open file
    // (selected) when there is one, else opening the vault folder — not the
    // in-app Sidebar tree. Item 42's original in-app behavior, and the
    // BottomBar -> Main -> Sidebar signal wiring it needed, are gone; the
    // reveal now goes through the DesktopIntegration singleton, which is what
    // carries the xdg-activation token that actually raises that window.
    // What each segment DOES on click is proven for real in
    // tst_bottombar_pathsegments (it clicks them against a recording stub);
    // DesktopIntegration's own call shape, token included, is covered by
    // tst_desktopintegration. This keeps only the cheap source-scan half.
    void bottomBar_fileLabelRevealsInFileManager()
    {
        const QString bar = readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml"));
        QVERIFY2(!bar.isEmpty(), "qml/BottomBar.qml must exist");
        QVERIFY2(bar.contains(QStringLiteral("DesktopIntegration.revealInFileManager")),
                 "the file segment must reveal the open file via "
                 "DesktopIntegration.revealInFileManager (item 42, reversed)");
        QVERIFY2(bar.contains(QStringLiteral("DesktopIntegration.openInFileManager")),
                 "folder segments must open their folder via "
                 "DesktopIntegration.openInFileManager (item 42, reversed)");
        QVERIFY2(!bar.contains(QStringLiteral("focusFileExplorerRequested")),
                 "BottomBar.qml must no longer route the label click to the in-app "
                 "Sidebar tree (item 42, reversed)");

        // Qt.openUrlExternally can only open a folder, never select a file
        // inside it, and hands the compositor no activation token — it is not a
        // substitute for the FileManager1 path.
        QVERIFY2(!bar.contains(QStringLiteral("Qt.openUrlExternally")),
                 "BottomBar.qml must go through DesktopIntegration, not "
                 "Qt.openUrlExternally (item 42, reversed)");

        const QString main = readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml"));
        QVERIFY2(!main.isEmpty(), "qml/Main.qml must exist");
        QVERIFY2(!main.contains(QStringLiteral("onFocusFileExplorerRequested")),
                 "Main.qml must no longer wire focusFileExplorerRequested (item 42, "
                 "reversed)");

        const QString side = readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml"));
        QVERIFY2(!side.isEmpty(), "qml/Sidebar.qml must exist");
        QVERIFY2(!side.contains(QStringLiteral("focusAndRevealNote")),
                 "Sidebar.qml's focusAndRevealNote() had no caller left once the label "
                 "click went back to the OS file manager — it must not linger as dead "
                 "code (item 42, reversed)");
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

        // The undo/redo STORAGE (both stacks + push/pop + redo-invalidation)
        // moved to src/core/UndoStack (2026-07-20): a per-panel instantiable
        // UndoStack element the MainPanel owns. The view-entangled APPLY step
        // (applyHistoryEntry) and the shortcuts stayed in QML.
        const QString undoSrc = readAll(QStringLiteral(SUOP_SRC_DIR "/core/UndoStack.cpp"));
        QVERIFY2(!undoSrc.isEmpty(), "src/core/UndoStack.cpp must exist");

        QVERIFY2(src.contains(QStringLiteral("UndoStack {")),
                 "MainPanel must own an instantiable UndoStack element (per-panel undo/redo "
                 "state, not a singleton) — item 46");
        QVERIFY2(undoSrc.contains(QStringLiteral("UndoStack::takeRedo")),
                 "UndoStack must own a redo stack (takeRedo) alongside the undo stack (item 46)");
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

        // Standard semantics: a fresh edit clears the redo branch. UndoStack::
        // push() is the single choke point for every user edit, so the clear
        // lives there. functionBody() (not a fixed mid() window) so the guard
        // can't silently rot as the method grows.
        const QString pushBody = functionBody(undoSrc, QStringLiteral("UndoStack::push"));
        QVERIFY2(!pushBody.isEmpty(), "UndoStack::push() must exist");
        QVERIFY2(pushBody.contains(QStringLiteral("m_redo.clear()")),
                 "UndoStack::push() must clear the redo stack — a fresh user edit invalidates the "
                 "redo branch (standard undo/redo semantics, item 46)");
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
        const QString reBlock = src.mid(reIdx, 900);
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

        // The UI must be reachable: find lives on the search bar (Ctrl+F),
        // and Ctrl+H is the shortcut that arms replace there.
        QVERIFY2(src.contains(QStringLiteral("searchExpanded")),
                 "the find/replace bar must be the search bar (searchExpanded), item 7");
        QVERIFY2(src.contains(QStringLiteral("Ctrl+H")),
                 "Ctrl+H must reach the replace row (item 7)");
    }

    // Item 7: find and replace are ONE bar — the search field is the find
    // pattern, and replace is armed by a checkbox rather than a second
    // toggle/icon of its own. Source-scan guard (no QQuickView here to drive
    // the UI) over the three invariants that keep that true.
    void mainPanel_replaceRidesOnTheSearchBar()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        // 1. No separate find state or find/replace bar toggle — searchQuery
        // is the one pattern, so a stray findText/findReplaceExpanded means
        // the two fields have drifted apart again.
        QVERIFY2(!src.contains(QStringLiteral("findText")),
                 "there must be no separate find pattern — searchQuery IS the find pattern (item 7)");
        QVERIFY2(!src.contains(QStringLiteral("findReplaceExpanded")),
                 "the find/replace bar must not have its own expanded bool — it is the search bar (item 7)");
        QVERIFY2(!src.contains(QStringLiteral("\"find-replace\"")),
                 "the standalone find-replace icon button is gone — replace is a checkbox on the "
                 "search bar (item 7)");

        // 2. The checkbox gates the actual mutation, not merely the row's
        // visibility: replaceNext()/replaceAll() must each bail when it's
        // off, so nothing can edit the transcript through an unarmed bar.
        for (const QString &fn : {QStringLiteral("function replaceNext("),
                                  QStringLiteral("function replaceAll(")}) {
            const int idx = src.indexOf(fn);
            QVERIFY2(idx >= 0, qPrintable(fn + " must exist (item 7)"));
            const QString body = src.mid(idx, 260);
            QVERIFY2(body.contains(QStringLiteral("if (!mainPanel.replaceEnabled)")),
                     qPrintable(fn + " must no-op unless the Replace checkbox is checked (item 7)"));
        }

        // 3. The search now runs through the SAME compiled pattern replace
        // does — otherwise the "a/b" count could describe different matches
        // than Replace all would touch.
        const int locIdx = src.indexOf(QStringLiteral("function locateMatch("));
        QVERIFY2(locIdx >= 0, "locateMatch() must exist");
        const QString locBlock = src.mid(locIdx, 900);
        QVERIFY2(locBlock.contains(QStringLiteral("findMatches(")),
                 "search must locate matches via findMatches()/buildFindRegex(), not its own "
                 "literal indexOf scan — one pattern for both search and replace (item 7)");
        QVERIFY2(!locBlock.contains(QStringLiteral("indexOf(q")),
                 "locateMatch() must not fall back to a literal indexOf scan (item 7)");
    }

    // Item 048 (symptom A): the sticky title RowLayout also hosts the
    // search/replace cluster once searchExpanded is true — clicking a
    // sidebar search-result snippet forces that expansion. On a narrow
    // panel, ~350px of fixed-width search controls beside a titleContainer
    // with no floor collapsed it toward 0 and clipped the title. No
    // QQuickView harness for MainPanel (too many C++ deps), so this is a
    // source-scan guard over the shrink+elide fix: titleContainer must
    // declare a Layout.minimumWidth (a floor to collapse toward instead of
    // overflowing), and searchField must be shrinkable rather than a bare
    // fixed Layout.preferredWidth: 200 (so it gives width back under
    // pressure instead of forcing the row to overflow).
    void mainPanel_titleContainerAndSearchFieldShrinkGracefully()
    {
        // mainPanelSrc(): searchField lives in SearchReplaceBar.qml now, while
        // titleContainer stays in MainPanel.qml — the concat covers both.
        const QString src = stripComments(mainPanelSrc());
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const QString titleContainer = enclosingBlock(src, QStringLiteral("id: titleContainer"));
        QVERIFY2(!titleContainer.isEmpty(), "id: titleContainer must exist in MainPanel.qml");
        QVERIFY2(titleContainer.contains(QStringLiteral("Layout.minimumWidth")),
                 "titleContainer must declare a Layout.minimumWidth so the sticky title row "
                 "gives it a floor to shrink toward instead of collapsing to 0 when the "
                 "search/replace cluster expands beside it (item 048)");

        const QString searchField = enclosingBlock(src, QStringLiteral("id: searchField"));
        QVERIFY2(!searchField.isEmpty(), "id: searchField must exist in MainPanel.qml");
        QVERIFY2(!searchField.contains(QStringLiteral("Layout.preferredWidth: 200")),
                 "searchField must not be a bare fixed Layout.preferredWidth: 200 — that forces "
                 "the title row to overflow on a narrow panel (item 048)");
        QVERIFY2(searchField.contains(QStringLiteral("Layout.fillWidth: true"))
                     || searchField.contains(QStringLiteral("Layout.minimumWidth")),
                 "searchField must be shrinkable (Layout.fillWidth and/or Layout.minimumWidth) "
                 "so it gives width back to titleContainer under pressure (item 048)");
    }

    // Item 41: note-list keyboard navigation (Up/Down, Shift+Up/Down,
    // Ctrl+Up/Down). No QQuickView in this suite to deliver key events to a
    // TreeView delegate, so these are source-scan guards that the Shortcuts,
    // their focus gating, and the backing functions all exist. (The old
    // Left/Right per-row action-button cluster nav was removed — those actions
    // live in the ribbon + MainPanel clusters now, see CLAUDE.md parity note.)

    // Shift+Up/Down extend the selection range from the anchor; Ctrl+Up/Down
    // jump to the current group's (parent-folder's) boundary — both gated on
    // the same view-focus condition as plain Up/Down.
    void sidebar_shiftAndCtrlArrowSelection()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("function extendNoteSelection(direction)")),
                 "extendNoteSelection() must back Shift+Up/Down range extension");
        QVERIFY2(src.contains(QStringLiteral("function jumpToGroupBoundary(direction)")),
                 "jumpToGroupBoundary() must back Ctrl+Up/Down group-boundary jumps");
        QVERIFY2(src.contains(QStringLiteral("function extendNoteSelectionToGroupBoundary(direction)")),
                 "extendNoteSelectionToGroupBoundary() must back Ctrl+Shift+Up/Down "
                 "(group jump + Shift-range in one gesture)");
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

        // Ctrl+Shift group-boundary extend must combine both: the fixed anchor +
        // moving cursor of a Shift-range, walked to a parent-folder boundary, and
        // must select the contiguous swept range (like extendNoteSelection).
        const int gExtIdx = src.indexOf(QStringLiteral("function extendNoteSelectionToGroupBoundary("));
        const QString gExtBody = src.mid(gExtIdx, 2000);
        QVERIFY2(gExtBody.contains(QStringLiteral("sidebar.lastClickedPath"))
                     && gExtBody.contains(QStringLiteral("sidebar.selectionCursorPath")),
                 "extendNoteSelectionToGroupBoundary must keep the fixed anchor and move the cursor");
        QVERIFY2(gExtBody.contains(QStringLiteral("sidebar.parentDir(")),
                 "extendNoteSelectionToGroupBoundary must stop at folder boundaries");
        QVERIFY2(gExtBody.contains(QStringLiteral("order.slice(lo, hi + 1)")),
                 "extendNoteSelectionToGroupBoundary must select the contiguous anchor..boundary range");

        // Both key pairs wired as Shortcuts, gated like plain Up/Down — via the
        // shared noteShortcutsEnabled now (see sidebar_arrowKeyNavigatesNotes
        // for why the focus test moved into that gate).
        for (const QString &seq : {QStringLiteral("Shift+Up"), QStringLiteral("Shift+Down"),
                                   QStringLiteral("Ctrl+Up"), QStringLiteral("Ctrl+Down"),
                                   QStringLiteral("Ctrl+Shift+Up"), QStringLiteral("Ctrl+Shift+Down")}) {
            const int sIdx = src.indexOf(QStringLiteral("sequence: \"%1\"").arg(seq));
            QVERIFY2(sIdx >= 0, qPrintable(seq + QStringLiteral(" must be wired as a Shortcut")));
            const QString sBlock = src.mid(sIdx, 220);
            QVERIFY2(sBlock.contains(QStringLiteral("sidebar.noteShortcutsEnabled")),
                     qPrintable(seq + QStringLiteral(" must be focus-gated exactly like plain Up/Down "
                                                     "(via noteShortcutsEnabled)")));
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

        const QString popup = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml")));
        QVERIFY2(!popup.isEmpty(), "qml/SettingsPopup.qml must exist");
        QVERIFY2(popup.contains(QStringLiteral("id: chapterDensitySlider")),
                 "SettingsPopup must expose a chapter-density slider (item 27)");
        QVERIFY2(popup.contains(QStringLiteral("AppSettings.chapterDensity = value")),
                 "the slider must write AppSettings.chapterDensity on move (item 27)");
        // The reset button was collapsed into the shared SettingResetButton
        // component (the slider itself stays inline). The row must instantiate it
        // for the chapterDensity property...
        const QRegularExpression resetBtn(
            QStringLiteral("SettingResetButton\\s*\\{\\s*propertyName:\\s*\"chapterDensity\""));
        QVERIFY2(resetBtn.match(popup).hasMatch(),
                 "the chapter-density row must have a reset-to-default button — "
                 "SettingResetButton { propertyName: \"chapterDensity\" } (item 27)");
        // ...and that component must actually revert/gate on the named property.
        const QString resetSrc = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/SettingResetButton.qml")));
        QVERIFY2(!resetSrc.isEmpty(), "qml/SettingResetButton.qml must exist");
        QVERIFY2(resetSrc.contains(QStringLiteral("AppSettings.resetProperty(root.propertyName)")),
                 "SettingResetButton must reset the named AppSettings property on click (item 27)");
        QVERIFY2(resetSrc.contains(QStringLiteral("AppSettings.isDefault(root.propertyName)")),
                 "SettingResetButton must grey out via AppSettings.isDefault(propertyName) (item 27)");

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
        const QString src = mainPanelSrc();
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
        const QString src = mainPanelSrc();
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("property real editingChapterTs")),
                 "a single editingChapterTs state must track which chapter title is being edited (item 34)");
        // The TRANSCRIPT heading (only) stays read-only-until-armed: it is an
        // overlay on transcriptArea whose title is swept into the body selection
        // and spliced into the clipboard by copySelectionWithChapters(), which an
        // always-editable field would swallow (StyleGuide's carve-out). The TOC
        // title was deliberately converted to an always-editable TextField in
        // queue item 025 — see mainPanel_tocChapterTitleIsAlwaysEditable — so it
        // is intentionally NOT asserted here anymore.
        QVERIFY2(src.contains(QStringLiteral("readOnly: !headingTitle.editing")),
                 "the in-transcript chapter heading must be readOnly except when it is the single "
                 "active edit (item 34) — it takes part in the transcript's own selection surface");
        QVERIFY2(src.contains(QStringLiteral("selectByMouse: headingTitle.editing")),
                 "the in-transcript chapter heading must only be mouse-selectable while actively "
                 "editing — no lingering multi-row selections (item 34)");
        // Clicking away commits+exits the active edit. functionBody(), not a
        // fixed-size mid() window — the window silently stopped reaching the
        // assertion the moment deselectTextSelections() grew (it also blurs the
        // identity fields now), which is the raw-indexOf/mid trap this file's
        // helpers exist to avoid.
        const QString deselBody = functionBody(src, QStringLiteral("function deselectTextSelections("));
        QVERIFY2(!deselBody.isEmpty(), "deselectTextSelections() must exist");
        QVERIFY2(deselBody.contains(QStringLiteral("editingChapterTs = -1")),
                 "clicking a non-interactive region must clear the active chapter edit (item 34)");
    }

    // Clicking a chapter title opens it for editing with the caret AT THE CLICK,
    // not with the whole title selected — a click-to-edit name is an invitation
    // to amend it, and a select-all means the next keystroke silently wipes it.
    // The in-transcript HEADING can't lean on the press to place the caret
    // (selectByMouse is false until it becomes the active edit), so it must set
    // cursorPosition itself. The TOC title is an always-editable TextField
    // (queue item 025) whose native press already places the caret — it must NOT
    // hand-place one. No QQuickView here, so this is a source scan.
    void mainPanel_chapterTitleClickDoesNotSelectAll()
    {
        const QString src = stripComments(mainPanelSrc());
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const QString toc = enclosingBlock(src, QStringLiteral("id: tocChapterTitle"));
        QVERIFY2(!toc.isEmpty(), "id: tocChapterTitle must exist in MainPanel.qml");
        QVERIFY2(!toc.contains(QStringLiteral("selectAll()")),
                 "clicking the TOC chapter title must not select the whole title");
        QVERIFY2(!toc.contains(QStringLiteral("positionAt(")),
                 "the always-editable TOC chapter title must not hand-place the caret — the field "
                 "owns its full width, so the native press does it (queue item 025)");

        const QString row = enclosingBlock(src, QStringLiteral("id: chapterHeadingRow"));
        QVERIFY2(!row.isEmpty(), "id: chapterHeadingRow must exist in MainPanel.qml");
        const QString begin = functionBody(row, QStringLiteral("function beginTitleEdit"));
        QVERIFY2(!begin.isEmpty(), "beginTitleEdit() must exist in chapterHeadingRow");
        QVERIFY2(!begin.contains(QStringLiteral("selectAll()")),
                 "clicking the in-transcript chapter heading must not select the whole title");
        QVERIFY2(begin.contains(QStringLiteral("headingTitle.positionAt(")),
                 "the chapter heading title must place the caret at the clicked position via positionAt()");
    }

    // A chapter title is NOT text in transcriptArea's document — displayData
    // reserves a zero-length blank line for it and an overlay field draws the
    // title — so one native selection can never span both. Selecting across
    // chapters and transcript is therefore synthesized: a transcript selection
    // sweeping a heading's reserved line selects that title too, and copy
    // splices the same titles back into the clipboard text.
    //
    // The two MUST key off the same predicate. If they ever drift, the bug is
    // silent and nasty: you copy something other than what you saw highlighted.
    // No QQuickView harness for MainPanel, so this is a source-scan guard.
    void mainPanel_chapterTitlesSelectAndCopyWithTheTranscript()
    {
        const QString src = stripComments(mainPanelSrc());
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        const QString sweeps = functionBody(src, QStringLiteral("function selectionSweepsHeading("));
        QVERIFY2(!sweeps.isEmpty(),
                 "selectionSweepsHeading() must exist — the single predicate deciding whether a "
                 "transcript selection covers a chapter heading");
        // Strictly-greater on the end: a selection stopping exactly AT the
        // heading's line hasn't reached it, and >= would drag in the next
        // chapter's title on every such selection.
        QVERIFY2(sweeps.contains(QStringLiteral("transcriptArea.selectionEnd > headingStart")),
                 "selectionSweepsHeading() must require the selection to extend PAST the heading's "
                 "zero-length line (>, not >=) — otherwise a selection ending at the previous "
                 "sentence silently swallows the next title");

        // The visible highlight is driven by that predicate...
        const QString title = enclosingBlock(src, QStringLiteral("id: headingTitle"));
        QVERIFY2(!title.isEmpty(), "id: headingTitle must exist");
        QVERIFY2(title.contains(QStringLiteral("mainPanel.selectionSweepsHeading(")),
                 "the heading title must mirror the transcript's selection via "
                 "selectionSweepsHeading() — a drag through the transcript cannot natively select it");
        QVERIFY2(title.contains(QStringLiteral("headingTitle.select(0, headingTitle.length)"))
                     && title.contains(QStringLiteral("headingTitle.deselect()")),
                 "a swept heading title must select (and un-select) its whole text — its reserved line "
                 "is zero-length, so there is no sub-title offset to select to");
        // ...and must never fight a real edit's own selection.
        QVERIFY2(title.contains(QStringLiteral("!headingTitle.editing")),
                 "the swept-selection mirror must be off while the title is being edited — the "
                 "selection is the user's then");
        // A mirrored highlight in a DIFFERENT colour doesn't read as one
        // selection at all. Unset means the Basic palette's opaque navy + white
        // text, which is what made a swept title look like an unrelated widget
        // lighting up instead of part of the sweep.
        QVERIFY2(title.contains(QStringLiteral("selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)")),
                 "the heading title must use the SAME selection colour as transcriptArea — a title "
                 "swept by a transcript selection has to look like part of that one selection");

        // ...and so is the copy.
        const QString copy = functionBody(src, QStringLiteral("function copySelectionWithChapters("));
        QVERIFY2(!copy.isEmpty(),
                 "copySelectionWithChapters() must exist — the native copy sees only transcriptArea's "
                 "document, where every chapter title is a blank line");
        QVERIFY2(copy.contains(QStringLiteral("mainPanel.selectionSweepsHeading(")),
                 "copy must splice in exactly the titles the highlight showed as selected — same "
                 "predicate, or you copy something you didn't see selected");
        QVERIFY2(copy.contains(QStringLiteral("Clipboard.setText(")),
                 "copy must put the spliced string on the clipboard via the Clipboard singleton");
        // Back-to-front splicing is what keeps the offsets valid: each insert
        // shifts everything after it.
        QVERIFY2(copy.contains(QStringLiteral("for (let i = headings.length - 1; i >= 0; --i)")),
                 "copy must splice titles back-to-front — a forward loop shifts the offsets of every "
                 "heading it hasn't reached yet");

        // The transcript must actually route Ctrl+C through it.
        const QString area = enclosingBlock(src, QStringLiteral("id: transcriptArea"));
        QVERIFY2(!area.isEmpty(), "id: transcriptArea must exist");
        QVERIFY2(area.contains(QStringLiteral("StandardKey.Copy"))
                     && area.contains(QStringLiteral("mainPanel.copySelectionWithChapters()")),
                 "transcriptArea must intercept Copy and route it through copySelectionWithChapters()");
    }

    // Item 33: chapters are collapsible in the transcript via a disclosure
    // affordance on the heading, and a collapsed chapter's body sentences are
    // filtered out of displayData (both the flowing text and displaySentences)
    // so the heading stays but its content hides. The whole-TOC collapse
    // (chaptersExpanded) must remain a distinct control. No QQuickView here,
    // so this is a source-scan guard.
    void mainPanel_transcriptChaptersCollapsible()
    {
        const QString src = mainPanelSrc();
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("property var collapsedChapters")),
                 "a per-chapter collapsed state must exist (item 33)");
        QVERIFY2(src.contains(QStringLiteral("function toggleChapterCollapse(")),
                 "a collapse toggle handler must exist (item 33)");
        // The toggle must be wired on the in-transcript heading row. Scoped to
        // the row's real braces, not a fixed char window from the id: — a
        // window long enough to reach the affordance today silently stops
        // reaching it the moment anything above it grows.
        const QString headingBody = enclosingBlock(src, QStringLiteral("id: chapterHeadingRow"));
        QVERIFY2(!headingBody.isEmpty(), "the in-transcript chapter heading row must exist");
        QVERIFY2(headingBody.contains(QStringLiteral("mainPanel.toggleChapterCollapse(")),
                 "the transcript heading must have a collapse affordance calling toggleChapterCollapse() "
                 "(item 33)");
        // displayData must actually drop collapsed-chapter sentences. The
        // assembly moved to src/core/TranscriptDisplayBuilder.cpp
        // (2026-07-20), so this now checks BOTH halves: MainPanel still feeds
        // the collapse map in, and the builder still skips the body. Scanned
        // against the real source rather than a fixed-width window — the
        // window this used to be rotted silently the moment silence phase 3's
        // divider logic landed above the check (queue item 085).
        const QString ddCall = bindingCall(stripComments(src),
                                           QStringLiteral("readonly property var displayData:"));
        QVERIFY2(!ddCall.isEmpty(), "displayData must exist");
        QVERIFY2(ddCall.contains(QStringLiteral("mainPanel.collapsedChapters")),
                 "MainPanel must feed the collapse map into the display-model builder (item 33)");
        const QString builder = stripComments(
            readAll(QStringLiteral(SUOP_SRC_DIR "/core/TranscriptDisplayBuilder.cpp")));
        QVERIFY2(!builder.isEmpty(), "src/core/TranscriptDisplayBuilder.cpp must exist");
        QVERIFY2(builder.contains(QStringLiteral("isCollapsed(currentChapterT0)"))
                     && builder.contains(QStringLiteral("continue;")),
                 "the builder must skip a collapsed chapter's body sentences so they hide from the "
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

    // Item 57: middle-click autoscroll is ONE shared component
    // (MiddleDragScroll.qml), used by all three scrollable surfaces. It used
    // to be three hand-copied implementations, and each copy independently
    // carried the same bug: the armed gesture was scoped to the panel it
    // started in, so the scroll silently froze the moment the cursor moved
    // onto anything else. tst_middledragscroll.cpp proves the behavior in a
    // real view; these are the cheap source-scan guards over the two Qt Quick
    // traps that caused it, both of which read as harmless in review.
    void qml_middleDragAutoscrollIsOneSharedComponent()
    {
        const QString component = readAll(QStringLiteral(SUOP_QML_DIR "/MiddleDragScroll.qml"));
        QVERIFY2(!component.isEmpty(), "qml/MiddleDragScroll.qml must exist");

        // Trap 1: `parent: Overlay.overlay` (attached to the tracker itself)
        // resolves to null before the item has a window, and that null
        // unparents the very item whose windowChanged would re-trigger the
        // binding — so it stays parentless and inert forever. It must resolve
        // against an item that stays in the scene.
        QVERIFY2(component.contains(QStringLiteral("parent: root.Overlay.overlay")),
                 "the armed-phase tracker must parent to root.Overlay.overlay — a bare "
                 "Overlay.overlay deadlocks itself to a null parent (item 57)");
        // Trap 2: gesture 2's held drag must survive leaving the arm region.
        // DragThreshold (the default) cancels the grab within a few px;
        // DragWithinBounds cancels it at the panel edge.
        QVERIFY2(component.contains(QStringLiteral("gesturePolicy: TapHandler.ReleaseWithinBounds")),
                 "the arming TapHandler must use ReleaseWithinBounds so a held middle-drag isn't "
                 "cancelled by drag distance or by leaving the arm region (item 57)");

        // The direct call sites use the component, and none re-implements it —
        // draggedWhileHeld/originY are its private gesture state, so any copy
        // of the old hand-rolled version would name them. Item 067: MainPanel's
        // transcript autoscroll is no longer a direct call site — the note page
        // is a ScrollPage, which bakes MiddleDragScroll in — so ScrollPage is
        // the base's own call site here; Sidebar/SettingsPopup still use it
        // directly.
        const QStringList users = {QStringLiteral("ScrollPage.qml"), QStringLiteral("Sidebar.qml"),
                                   QStringLiteral("SettingsPopup.qml")};
        for (const QString &file : users) {
            const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/") + file);
            QVERIFY2(src.contains(QStringLiteral("MiddleDragScroll {")),
                     qPrintable(file + QStringLiteral(" must use the shared MiddleDragScroll "
                                                      "component for middle-click autoscroll (item 57)")));
            QVERIFY2(!stripComments(src).contains(QStringLiteral("draggedWhileHeld")),
                     qPrintable(file + QStringLiteral(" must not re-implement the autoscroll gesture — "
                                                      "MiddleDragScroll.qml owns it (item 57)")));
        }

        // MainPanel must not re-implement the gesture either — it gets the
        // transcript's autoscroll from ScrollPage.
        const QString mainPanel = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!stripComments(mainPanel).contains(QStringLiteral("draggedWhileHeld")),
                 "MainPanel must not re-implement the autoscroll gesture — ScrollPage/"
                 "MiddleDragScroll owns it (item 57)");

        // MainPanel's arm region is still deliberately the whole note page, not
        // just the transcript box: a middle-click on the tag pills / prelude /
        // chapter list should arm the transcript's scroll too. That reach is now
        // structural — the note page is a ScrollPage that anchors.fill: parent,
        // and ScrollPage's built-in MiddleDragScroll fills the whole page.
        const QString noteBlock = enclosingBlock(stripComments(mainPanel), QStringLiteral("id: notePage"));
        QVERIFY2(!noteBlock.isEmpty(), "the note page (id: notePage) must exist");
        QVERIFY2(noteBlock.contains(QStringLiteral("anchors.fill: parent")),
                 "the note page must anchors.fill its panel so ScrollPage's built-in middle-drag "
                 "arms autoscroll from the whole note page (item 57)");
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

        // The stale transcript is dimmed AND non-interactive while shown. Item
        // 067 moved these bindings onto the scrolling content column, since
        // ScrollPage owns the inner ScrollView now.
        const QString svBlock = enclosingBlock(stripComments(src), QStringLiteral("id: scrollableContent"));
        QVERIFY2(!svBlock.isEmpty(), "scrollableContent must exist (item 55)");
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
    // label/prelude/chapters buttons gate on hasMetadata, which flips
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
                 "the live-content tick must refresh metadata for the open file so hasMetadata "
                 "flips true (regenerate buttons appear) on a file's first transcription (item 32)");
    }

    // Items 24/53 + CLAUDE.md per-file-action-parity: "run all jobs"
    // (transcribe->detect speakers->label->chapters) must exist in the required
    // button sets — Sidebar's ribbon (bulk), Sidebar's search-scoped run-all
    // group, and MainPanel's icon cluster (open file) — each wired to the
    // runAllJobs()/runAllJobsForPaths() controller invokables. (The per-row
    // hover cluster was removed — CLAUDE.md parity is now two sets, not three.)
    // No QQuickView here, so this is a source-scan guard.
    void sidebar_runAllJobsActionParityAcrossAllSites()
    {
        // sidebarTreeSrc(): the ribbon button flow lives in SidebarRibbon.qml now.
        const QString sidebar = sidebarTreeSrc();
        QVERIFY2(!sidebar.isEmpty(), "qml/Sidebar.qml must exist");
        const QString mainPanel = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!mainPanel.isEmpty(), "qml/MainPanel.qml must exist");

        // (1) Ribbon bulk run-all button, next to transcribeAll/relabelAll/
        // chaptersAll, narrowing via the model-derived selectionTranscribePaths
        // (item 041) and running the bulk controller entry point.
        const int ribbonIdx = sidebar.indexOf(QStringLiteral("id: runAllButton"));
        QVERIFY2(ribbonIdx >= 0, "Sidebar ribbon must have a bulk runAllButton (items 24/53)");
        const QString ribbonBlock = sidebar.mid(ribbonIdx, 1200);
        QVERIFY2(ribbonBlock.contains(QStringLiteral("sidebar.selectionTranscribePaths")),
                 "the ribbon run-all button must narrow to the selection via selectionTranscribePaths");
        QVERIFY2(ribbonBlock.contains(QStringLiteral("transcriptionController.runAllJobsForPaths(")),
                 "the ribbon run-all button must call the bulk runAllJobsForPaths() invokable");

        // (2) Search-scoped run-all button + its eligibility helper, inside the
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

        // (3) MainPanel icon cluster run-all button — per the user's spec,
        // shown only when there's no transcription yet (untranscribed open
        // file), and wired to runAllJobs() for that file.
        const int mpIdx = mainPanel.indexOf(QStringLiteral("id: runAllIconButton"));
        QVERIFY2(mpIdx >= 0, "MainPanel must have a runAllIconButton (items 24/53)");
        const QString mpBlock = mainPanel.mid(mpIdx, 1500);
        QVERIFY2(mpBlock.contains(QStringLiteral("!mainPanel.hasMetadata")),
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

    // Item 2 (delete transcript + delete file): the destructive file actions
    // must reach the user via the Sidebar context menu, the Sidebar RIBBON, and
    // the MainPanel open-file icon cluster (CLAUDE.md per-file-action parity).
    //
    // The ribbon half is a REVERSAL, recorded here so it isn't quietly undone:
    // a bulk ribbon delete was previously left out on purpose, as "too
    // dangerous for a multi-selection". That call was made while a delete was
    // confirm-then-maybe-permanent. It isn't anymore — every delete path here
    // now tries the trash FIRST (see Sidebar's handleTrashResult), so the
    // recoverable case costs an Undo click from the snackbar rather than the
    // file, and only a filesystem refusal is ever permanent. The Delete key
    // also needs a visible affordance to advertise. No QQuickView here, so this
    // is a source-scan guard that the wiring exists.
    void delete_transcriptAndFile_wiredInSidebarAndMainPanel()
    {
        // sidebarTreeSrc(): the ribbon button flow lives in SidebarRibbon.qml now.
        const QString sidebar = sidebarTreeSrc();
        QVERIFY2(!sidebar.isEmpty(), "qml/Sidebar.qml must exist");

        // Stripped: every assertion below is a PRESENCE one, and those pass on
        // a comment alone — this block's own doc comment names most of these
        // calls, so an unstripped scan would go green on documentation.
        const QString sidebarCode = stripComments(sidebar);

        // Context-menu entries for both actions.
        QVERIFY2(sidebarCode.contains(QStringLiteral("id: ctxDeleteTranscriptRow")),
                 "Sidebar context menu must offer a 'Delete transcript' entry for transcribed files");
        QVERIFY2(sidebarCode.contains(QStringLiteral("id: ctxDeleteFileRow")),
                 "Sidebar context menu must offer a 'Delete file' entry for note rows");
        QVERIFY2(sidebarCode.contains(QStringLiteral("id: ctxDeleteAudioOnlyRow")),
                 "Sidebar context menu must offer a 'Delete audio only' entry (drops the audio, "
                 "keeps the .md) for transcribed files");
        // "Delete transcript" is gated to a transcribed AUDIO row (status 2).
        QVERIFY2(sidebarCode.contains(QStringLiteral("visible: sidebar.ctxIsAudio && sidebar.ctxStatus === 2")),
                 "'Delete transcript' must only show for a transcribed audio row (an orphaned .md gets "
                 "'Delete file' instead)");
        // Both route through the model's TRASH-first invokables. A delete only
        // reaches a confirm popup if the trash refused (see Sidebar's
        // handleTrashResult) — the recoverable case offers Undo instead.
        QVERIFY2(sidebarCode.contains(QStringLiteral(".trashTranscript(")),
                 "Sidebar must call VaultTreeModel.trashTranscript()");
        QVERIFY2(sidebarCode.contains(QStringLiteral(".trashAudioOnly(")),
                 "Sidebar must call VaultTreeModel.trashAudioOnly() — the trash-first call, not "
                 "deleteAudioOnlyPermanently()");
        // Every delete call takes a LIST: the sidebar's deletes act on a whole
        // selection, and a single row is just a one-element batch — one code
        // path, not a count-dependent branch.
        QVERIFY2(sidebarCode.contains(QStringLiteral(".trashFiles(")),
                 "Sidebar must delete through VaultTreeModel.trashFiles(), which handles both a single "
                 "row and a multi-selection in one rebuild");
        // Reached ONLY when the trash refused — that's the one permanent case.
        QVERIFY2(sidebarCode.contains(QStringLiteral("id: fileDeleteConfirmPopup")),
                 "a delete the trash refused (the permanent case) must be confirmed first");

        // The ribbon's bulk trio — the CLAUDE.md parity site for MainPanel's
        // per-file cluster, and the Delete key's visible affordance. See this
        // slot's doc comment for why these now exist.
        for (const QString &id : { QStringLiteral("deleteTranscriptAllButton"),
                                    QStringLiteral("deleteAudioOnlyAllButton"),
                                    QStringLiteral("deleteFileAllButton") }) {
            QVERIFY2(sidebarCode.contains(QStringLiteral("id: ") + id),
                     qPrintable(QStringLiteral("Sidebar ribbon must have a bulk %1 — CLAUDE.md per-file "
                                                "action parity with MainPanel's delete cluster").arg(id)));
        }
        // Bulk => must narrow to the current selection like every other ribbon
        // button, never act on the whole vault.
        //
        // enclosingBlock(), not functionBody(): the anchor is an "id:" line,
        // which sits INSIDE the block being asked about, so the brace that
        // opens it is BEHIND the anchor. functionBody() searches forward and
        // would brace-match the button's first child handler instead.
        QVERIFY2(enclosingBlock(sidebarCode, QStringLiteral("id: deleteFileAllButton"))
                     .contains(QStringLiteral("sidebar.selectedPaths")),
                 "the ribbon delete-file button must act on the selection");
        for (const QString &id : { QStringLiteral("id: deleteTranscriptAllButton"),
                                    QStringLiteral("id: deleteAudioOnlyAllButton") }) {
            QVERIFY2(enclosingBlock(sidebarCode, id).contains(QStringLiteral("sidebar.selectionRelabelPaths")),
                     qPrintable(QStringLiteral("%1 must narrow to the selection via selectionRelabelPaths (item 041)").arg(id)));
        }

        const QString mainPanel = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(!mainPanel.isEmpty(), "qml/MainPanel.qml must exist");
        const QString mainPanelCode = stripComments(mainPanel);

        QVERIFY2(mainPanelCode.contains(QStringLiteral("id: deleteTranscriptButton")),
                 "MainPanel icon cluster must have a 'Delete transcript' action for the open file");
        QVERIFY2(mainPanelCode.contains(QStringLiteral("id: deleteFileButton")),
                 "MainPanel icon cluster must have a 'Delete file' action for the open file");
        // Per-file action parity (CLAUDE.md): "Delete audio only" is offered on
        // the vault tree's rows, so the open file must offer it too.
        QVERIFY2(mainPanelCode.contains(QStringLiteral("id: deleteAudioOnlyButton")),
                 "MainPanel icon cluster must have a 'Delete audio only' action for the open file, "
                 "matching the Sidebar's ctxDeleteAudioOnlyRow");
        // With a controller the open-file deletes go through the async
        // TrashTask (transcriptionController.trash) — undoable via the generic
        // snackbar, visible in the Tasks list, deferred behind a same-file
        // transcribe. The synchronous VaultTreeModel.trash* calls stay as the
        // no-controller fallback (plain-model harnesses).
        QVERIFY2(mainPanelCode.contains(QStringLiteral("transcriptionController.trash(")),
                 "MainPanel's open-file deletes (with a controller) must route through "
                 "TranscriptionController.trash() — the async, undoable instant-lane task");
        QVERIFY2(mainPanelCode.contains(QStringLiteral(".trashTranscript("))
                     && mainPanelCode.contains(QStringLiteral(".trashAudioOnly("))
                     && mainPanelCode.contains(QStringLiteral(".trashFiles(")),
                 "MainPanel must still fall back to VaultTreeModel.trashTranscript()/trashAudioOnly()/"
                 "trashFiles() — the trash-first calls, not the *Permanently() ones — with no controller");
        // Still present, but now reached ONLY when the trash refused.
        QVERIFY2(mainPanelCode.contains(QStringLiteral("id: deleteFileConfirmPopup")),
                 "MainPanel must still confirm a delete the trash refused (the permanent case)");
        QVERIFY2(mainPanel.contains(QStringLiteral("function handleTrashResult(")),
                 "MainPanel's deletes must funnel through handleTrashResult(), same trash-first "
                 "flow as the Sidebar's");
        // When the OPEN file is deleted, the view must clear/refresh — driven
        // off the model's transcriptDeleted/fileDeleted signals so it works
        // whether the delete was triggered here or from the Sidebar.
        QVERIFY2(mainPanel.contains(QStringLiteral("onTranscriptDeleted"))
                     && mainPanel.contains(QStringLiteral("onFileDeleted")),
                 "MainPanel must react to VaultTreeModel.transcriptDeleted/fileDeleted so the open file's "
                 "view refreshes (revert) or clears (deleted) gracefully");
        // Deleting the audio TO KEEP the transcript, then closing that
        // transcript, defeats the action. The panel must follow the .md over
        // instead — and via the signal, so a Sidebar-triggered delete of the
        // open file does it too.
        const QString audioKept = functionBody(
            mainPanel, QStringLiteral("function onAudioDeletedTranscriptKept("));
        QVERIFY2(!audioKept.isEmpty(),
                 "MainPanel must handle VaultTreeModel.audioDeletedTranscriptKept — otherwise "
                 "\"delete audio only\" closes the very transcript it exists to preserve");
        QVERIFY2(audioKept.contains(QStringLiteral("showOrphanedMd(")),
                 "on an audio-only delete the panel must follow the surviving transcript via "
                 "showOrphanedMd(), not clear the view");
    }

    // Item 27 + chapterDensity plumbing: every generateChapters()/
    // generateChaptersAll() QML call site in Sidebar.qml must forward
    // AppSettings.chapterDensity (matching MainPanel's call site), not rely on
    // the C++ default 0.5 — otherwise the density slider silently does nothing
    // for chapters kicked off from the sidebar.
    void sidebar_generateChaptersCallsPassChapterDensity()
    {
        // sidebarTreeSrc(): the ribbon button flow lives in SidebarRibbon.qml now.
        const QString sidebar = sidebarTreeSrc();
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
        QVERIFY2(found >= 2,
                 "expected both Sidebar generateChapters call sites (ribbon chaptersAll, search "
                 "chapters) to be present — the per-row/keyboard-nav sites were removed with the "
                 "per-row hover cluster");
    }

    // Item 52 (CLAUDE.md per-file-action parity): the standalone "detect
    // speakers" (diarization) action must exist in the Sidebar sites mirroring
    // MainPanel's detectSpeakersButton — the ribbon bulk button and the
    // search-scoped run group. (The per-row hover cluster was removed —
    // CLAUDE.md parity is now two sets.) Ribbon/search bulk sites call
    // diarizeFilesForPaths().
    void sidebar_detectSpeakersActionParityAcrossAllSites()
    {
        // sidebarTreeSrc(): the ribbon button flow lives in SidebarRibbon.qml now.
        const QString sidebar = sidebarTreeSrc();
        QVERIFY2(!sidebar.isEmpty(), "qml/Sidebar.qml must exist");

        // (1) Ribbon bulk detect-speakers button — over the transcribed set
        // (relabel-all's eligibility, since diarization needs a transcript).
        const int ribbonIdx = sidebar.indexOf(QStringLiteral("id: detectSpeakersAllButton"));
        QVERIFY2(ribbonIdx >= 0, "Sidebar ribbon must have a bulk detectSpeakersAllButton (item 52)");
        const QString ribbonBlock = sidebar.mid(ribbonIdx, 1200);
        QVERIFY2(ribbonBlock.contains(QStringLiteral("sidebar.selectionRelabelPaths")),
                 "the ribbon detect-speakers button must operate over the TRANSCRIBED set "
                 "(diarization needs an existing transcript — selectionRelabelPaths, item 041)");
        QVERIFY2(ribbonBlock.contains(QStringLiteral("transcriptionController.diarizeFilesForPaths(")),
                 "the ribbon detect-speakers button must call the bulk diarizeFilesForPaths() invokable");

        // (2) Search-scoped detect-speakers button + its eligibility helper.
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

    // Regression: "regenerate chapters" was reachable from the
    // per-file-action-parity sites, but every one of them gated on
    // relabelEligible (isAudio required), so an orphaned .md (audio deleted,
    // text/transcript still present) could never reach it — even though
    // TranscriptionController::generateChaptersJob() only ever reads the
    // .md's existing frontmatter/body and never touches audio (same as
    // relabelJob(), which MainPanel's regenerateLabelButton already exercises
    // for orphaned .md via orphanedMdPath/targetUrl). searchChaptersPaths()
    // already expected `relabelEligible || isOrphanedMd` for chapters
    // eligibility (see its existing body) — the ribbon/MainPanel sites just
    // hadn't been wired to match. No QQuickView here, so this is a source-scan
    // guard.
    void generateChapters_orphanedMdEligibleAcrossAllSites()
    {
        const QString sidebar = sidebarTreeSrc();
        QVERIFY2(!sidebar.isEmpty(), "qml/Sidebar.qml must exist");
        // mainPanelSrc(), not the raw file: the chapter TOC lives in
        // ChapterTocList.qml since its extraction (the shim appends it).
        const QString mainPanel = mainPanelSrc();
        QVERIFY2(!mainPanel.isEmpty(), "qml/MainPanel.qml must exist");

        // treeDelegate's own chaptersEligible must be a strict superset of
        // relabelEligible, adding isOrphanedMd — matching the inline
        // `relabelEligible || isOrphanedMd` expression the search-results
        // delegate already uses for this same purpose.
        const int eligIdx = sidebar.indexOf(QStringLiteral("readonly property bool chaptersEligible"));
        QVERIFY2(eligIdx >= 0, "treeDelegate must expose a chaptersEligible property");
        const QString eligLine = sidebar.mid(eligIdx, 200);
        QVERIFY2(eligLine.contains(QStringLiteral("treeDelegate.relabelEligible"))
                     && eligLine.contains(QStringLiteral("treeDelegate.isOrphanedMd")),
                 "chaptersEligible must be relabelEligible || isOrphanedMd, not relabelEligible alone");

        // (1) Ribbon bulk chaptersAllButton must run over an orphaned-inclusive
        // set (selectionChaptersPaths), not the audio-only selectionRelabelPaths.
        const int ribbonIdx = sidebar.indexOf(QStringLiteral("id: chaptersAllButton"));
        QVERIFY2(ribbonIdx >= 0, "Sidebar ribbon must have a bulk chaptersAllButton");
        const QString ribbonBlock = sidebar.mid(ribbonIdx, 1200);
        QVERIFY2(ribbonBlock.contains(QStringLiteral("sidebar.selectionChaptersPaths"))
                     && !ribbonBlock.contains(QStringLiteral("sidebar.selectionRelabelPaths")),
                 "the ribbon chapters button must target selectionChaptersPaths (orphaned-inclusive), "
                 "not selectionRelabelPaths (audio-only) — item 041");

        // (2) MainPanel's generateChaptersButton must support orphaned .md the
        // same way regenerateLabelButton already does: gated on hasTranscript
        // (true for an orphaned .md, not just transcribed audio) and dispatching
        // against a targetUrl that resolves to currentMdUrl — the single .md
        // identity that points at the orphaned path OR the audio sibling.
        const int mpIdx = mainPanel.indexOf(QStringLiteral("id: generateChaptersButton"));
        QVERIFY2(mpIdx >= 0, "MainPanel must have a generateChaptersButton");
        const QString mpBlock = mainPanel.mid(mpIdx, 1000);
        QVERIFY2(mpBlock.contains(QStringLiteral("mainPanel.hasTranscript")),
                 "generateChaptersButton must be gated on hasTranscript (which includes an orphaned "
                 ".md), not force-hidden when audio is absent");
        QVERIFY2(mpBlock.contains(QStringLiteral("mainPanel.currentMdUrl")),
                 "generateChaptersButton's targetUrl must resolve to currentMdUrl (the orphaned path "
                 "or the audio sibling), mirroring regenerateLabelButton");

        // refreshPreludeAndChapters() must populate mainPanel.chapters for an
        // orphaned .md too, so a freshly-generated chapter list becomes visible
        // right after the job's onFinished fires — it reads through currentMdUrl,
        // which is the orphaned path when one is open.
        const int refreshIdx = mainPanel.indexOf(QStringLiteral("function refreshPreludeAndChapters()"));
        QVERIFY2(refreshIdx >= 0, "refreshPreludeAndChapters() must exist");
        const QString refreshBlock = mainPanel.mid(refreshIdx, 700);
        QVERIFY2(refreshBlock.contains(QStringLiteral("mainPanel.currentMdUrl")),
                 "refreshPreludeAndChapters() must read prelude/chapters via currentMdUrl (so an "
                 "orphaned .md is covered), not only via player.source");

        // The chapter list itself must not be force-hidden for orphaned .md
        // (it only ever needs mainPanel.chapters, which the above now
        // populates for orphaned files too).
        const int listIdx = mainPanel.indexOf(QStringLiteral("id: chapterListColumn"));
        QVERIFY2(listIdx >= 0, "chapterListColumn must exist");
        const QString listBlock = mainPanel.mid(listIdx, 200);
        QVERIFY2(!listBlock.contains(QStringLiteral("mainPanel.orphanedMdPath ===")),
                 "chapterListColumn must not be gated shut for orphaned .md files");
    }

    // Regression: regenerateLabelButton/generateChaptersButton/detectSpeakersButton
    // used to be gated on hasTranscript/hasMetadata alone, so they stayed hidden
    // (or greyed via the coarse per-file busyOrQueuedUrls) for the entire span of
    // an in-flight transcription — a derived pass queued during that window was
    // either invisible or blocked by the transcribe itself occupying the same
    // audio url. transcribeInFlight + the per-kind busyOrQueuedKeys fix both: the
    // buttons must appear (and gate only on THEIR OWN kind) the instant a
    // transcribe starts. No QQuickView here, so this is a source-scan guard.
    void mainPanel_derivedButtonsAvailableDuringTranscription()
    {
        const QString mainPanel = stripComments(mainPanelSrc());
        QVERIFY2(!mainPanel.isEmpty(), "qml/MainPanel.qml must exist");

        const int keysIdx = mainPanel.indexOf(QStringLiteral("property var busyOrQueuedKeys"));
        QVERIFY2(keysIdx >= 0, "MainPanel must expose a busyOrQueuedKeys property");
        const QString keysBlock = mainPanel.mid(keysIdx, 400);
        QVERIFY2(keysBlock.contains(QStringLiteral("c.jobs")),
                 "busyOrQueuedKeys must be derived from c.jobs (the per-kind data source)");

        const int inFlightIdx = mainPanel.indexOf(QStringLiteral("property bool transcribeInFlight"));
        QVERIFY2(inFlightIdx >= 0, "MainPanel must expose a transcribeInFlight property");
        const QString inFlightBlock = mainPanel.mid(inFlightIdx, 300);
        QVERIFY2(inFlightBlock.contains(QStringLiteral("\"transcribe\"")),
                 "transcribeInFlight must key on the \"transcribe\" kind");

        auto checkButton = [&](const QString &id, const QString &kind) {
            const int idx = mainPanel.indexOf(QStringLiteral("id: %1").arg(id));
            QVERIFY2(idx >= 0, qPrintable(QStringLiteral("MainPanel must have %1").arg(id)));
            const QString block = mainPanel.mid(idx, 400);
            QVERIFY2(block.contains(QStringLiteral("mainPanel.transcribeInFlight")),
                     qPrintable(QStringLiteral("%1 must be visible while transcribeInFlight, not force-hidden "
                                                "for the whole span of an in-flight transcription")
                                    .arg(id)));
            QVERIFY2(block.contains(QStringLiteral("busyOrQueuedKeys")) && block.contains(QStringLiteral("\"%1\"").arg(kind)),
                     qPrintable(QStringLiteral("%1 must gate enabled on busyOrQueuedKeys with its own \"%2\" kind, "
                                                "not the per-file busyOrQueuedUrls")
                                    .arg(id, kind)));
        };
        checkButton(QStringLiteral("regenerateLabelButton"), QStringLiteral("relabel"));
        checkButton(QStringLiteral("generateChaptersButton"), QStringLiteral("chapters"));
        checkButton(QStringLiteral("detectSpeakersButton"), QStringLiteral("diarize"));
    }

    // The app mounts external volumes itself (KDE's automounter is off by default, so
    // a plugged-in recorder can otherwise sit unmounted forever and never grab). What
    // keeps that from making suopTranscriber a general-purpose automounter is that
    // mountVolume() is called from exactly ONE place, behind an enabled rule — the
    // same predicate the grab itself uses.
    //
    // A source scan rather than a unit test: UsbGrabberController constructs its
    // UsbDeviceMonitor internally (no seam to inject a fake through), and the real one
    // needs a live UDisks2 plus hardware to hotplug, so the composition
    // "unmountedVolumeDetected -> rule check -> mount" has no direct test path. The
    // pieces either side are covered — the external-media gate that decides WHAT may
    // be mounted in tst_usbdevicemonitor, rule resolution in tst_usbgrabbercontroller.
    void usbGrabber_mountIsRuleGated()
    {
        const QString src = readAll(QStringLiteral(SUOP_SRC_DIR "/core/UsbGrabberController.cpp"));
        QVERIFY2(!src.isEmpty(), "src/core/UsbGrabberController.cpp must exist");

        const int idx = src.indexOf(QStringLiteral("mountVolume("));
        QVERIFY2(idx >= 0, "UsbGrabberController must still mount unmounted volumes itself — nothing on a "
                            "stock KDE install is obliged to auto-mount them for us");
        QVERIFY2(src.indexOf(QStringLiteral("mountVolume("), idx + 1) < 0,
                 "mountVolume() must be called from exactly one place: every added call site is another "
                 "way for the app to mount something no rule ever asked for");

        // The gate itself, in the ~15 lines above the call.
        const int gateIdx = src.lastIndexOf(QStringLiteral("resolveEffectiveRule("), idx);
        QVERIFY2(gateIdx >= 0 && src.mid(gateIdx, idx - gateIdx).contains(QStringLiteral(".enabled")),
                 "mountVolume() must be gated on resolveEffectiveRule(...).enabled — the app may only mount "
                 "devices it would actually grab from, never every external volume it sees");

        // The monitor offers the candidate; it must never decide to mount one itself.
        const QString monitor = readAll(QStringLiteral(SUOP_SRC_DIR "/core/UsbDeviceMonitor.cpp"));
        QVERIFY2(!monitor.isEmpty(), "src/core/UsbDeviceMonitor.cpp must exist");
        const int emitIdx = monitor.indexOf(QStringLiteral("emit unmountedVolumeDetected("));
        QVERIFY2(emitIdx >= 0, "UsbDeviceMonitor must still report unmounted external volumes");
        QVERIFY2(!monitor.mid(emitIdx, 200).contains(QStringLiteral("mountVolume(")),
                 "UsbDeviceMonitor must not mount off the back of its own unmountedVolumeDetected signal: it "
                 "holds no rules, so it can only answer WHAT may be mounted, never WHETHER it should be");
    }

    // Dropped files must reach the vault as QUEUED TASKS, exactly like a USB
    // grab — never as a copy done inline on the drop handler. The copy is
    // unbounded (a dropped file can be gigabytes), so doing it here would
    // freeze the UI for its whole duration and leave the user with no Tasks
    // row, no progress, and no cancel. tst_transcription_queue.cpp specs the
    // import JOB itself; this pins the drop path actually going through it.
    void mainWindow_droppedFilesBecomeQueuedImportTasks()
    {
        // stripComments + functionBody: Main.qml's comments here name these very
        // calls while explaining them, and a `contains` guard that a comment can
        // satisfy guards nothing (see stripComments()' own doc comment).
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Main.qml must exist");

        const int dropIdx = src.indexOf(QStringLiteral("onDropped:"));
        QVERIFY2(dropIdx >= 0, "Main.qml must still accept OS file drops");
        QVERIFY2(src.mid(dropIdx, 120).contains(QStringLiteral("importDroppedUrls(")),
                 "the drop handler must delegate to importDroppedUrls()");

        const QString fn = functionBody(src, QStringLiteral("function importDroppedUrls("));
        QVERIFY2(!fn.isEmpty(), "Main.qml must define importDroppedUrls()");

        QVERIFY2(fn.contains(QStringLiteral("transcriptionController.importAudio(")),
                 "importDroppedUrls() must enqueue each file via TranscriptionController.importAudio() — the "
                 "byte-copy belongs on the worker thread's job queue, never inline on the GUI thread");
        QVERIFY2(fn.contains(QStringLiteral("inspectImportCandidate(")),
                 "importDroppedUrls() must validate synchronously via inspectImportCandidate() before enqueuing: "
                 "wrong-type/collision/no-vault deserve instant feedback, not a Failed task later");
    }

    // Settings saves are debounced by AppSettings' saveTimer, but the window
    // closes via TranscriptionController::terminateApplicationImmediately()'s
    // hard ::_exit(0) — which never lets that timer fire. So onClosing MUST
    // flush the pending save first, or any setting changed within the timer's
    // interval of closing is silently lost (the "titanet embedding model won't
    // persist" bug: it was just whatever the user touched last before closing).
    void mainWindow_closeFlushesPendingSettingsSave()
    {
        // stripComments + functionBody: both files' comments name these very
        // calls while explaining the bug, so a raw `contains` would pass on the
        // prose even if the code stopped doing it (see stripComments()' doc).
        const QString main = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml")));
        QVERIFY2(!main.isEmpty(), "qml/Main.qml must exist");

        const QString onClosing = functionBody(main, QStringLiteral("onClosing:"));
        QVERIFY2(!onClosing.isEmpty(), "Main.qml must still handle onClosing");
        const int flushIdx = onClosing.indexOf(QStringLiteral("AppSettings.flushPendingSave()"));
        const int termIdx = onClosing.indexOf(QStringLiteral("terminateApplicationImmediately()"));
        QVERIFY2(flushIdx >= 0,
                 "onClosing must flush pending settings via AppSettings.flushPendingSave() — the hard "
                 "::_exit(0) that follows never lets the debounced saveTimer fire");
        QVERIFY2(termIdx < 0 || flushIdx < termIdx,
                 "flushPendingSave() must run BEFORE terminateApplicationImmediately() — after the "
                 "::_exit(0) it would never run at all");

        // And flushPendingSave() must actually WRITE, not just stop the timer.
        const QString settings = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/AppSettings.qml")));
        const QString flush = functionBody(settings, QStringLiteral("function flushPendingSave()"));
        QVERIFY2(!flush.isEmpty(), "AppSettings.qml must define flushPendingSave()");
        QVERIFY2(flush.contains(QStringLiteral("saveToVault()")),
                 "flushPendingSave() must call saveToVault() — stopping the timer alone drops the change");
    }

    // A model picker's currentIndex must survive its list being assigned AFTER
    // the combo is built, and must never render an empty box. indexOfValue()
    // reads the model but that read is NOT a tracked QML binding dependency, so
    // `currentIndex: indexOfValue(selectedId)` alone computes -1 once against the
    // still-empty list and never re-runs when the list fills in — the "the
    // transcription-model box is empty on load even though the model IS saved"
    // bug. The binding must (a) touch `count` so it re-evaluates on list change,
    // and (b) fall back to the first entry (0) rather than -1.
    void modelComboBox_currentIndexReevaluatesAndDefaultsToFirst()
    {
        for (const QString &file : {QStringLiteral("ModelComboBox.qml"),
                                    QStringLiteral("SettingsPopup.qml")}) {
            // stripComments: these files' comments explain the `count`/fallback
            // trick BY NAME, so a raw scan would pass on the prose alone.
            const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/") + file));
            QVERIFY2(!src.isEmpty(), qPrintable(file + " must exist"));

            // Every currentIndex binding driven by indexOfValue must be guarded.
            int from = 0;
            int found = 0;
            while (true) {
                const int idx = src.indexOf(QStringLiteral("indexOfValue("), from);
                if (idx < 0)
                    break;
                ++found;
                // Walk back to the enclosing `currentIndex:` and brace-match its block.
                const int ci = src.lastIndexOf(QStringLiteral("currentIndex:"), idx);
                QVERIFY2(ci >= 0, qPrintable(file + ": indexOfValue() must sit inside a currentIndex binding"));
                const QString binding = functionBody(src, src.mid(ci, idx - ci));
                QVERIFY2(!binding.isEmpty(),
                         qPrintable(file + ": currentIndex must be a {…} block, not a bare indexOfValue()"));
                QVERIFY2(binding.contains(QStringLiteral("count")),
                         qPrintable(file + ": currentIndex must read `count` so the binding re-evaluates when "
                                           "the model list populates — otherwise the box stays empty at -1"));
                QVERIFY2(binding.contains(QStringLiteral("0 :")),
                         qPrintable(file + ": currentIndex must fall back to the first entry (0), never leave "
                                           "an empty box, when selectedId isn't in the list"));
                from = idx + 1;
            }
            QVERIFY2(found > 0, qPrintable(file + " was expected to drive a model picker via indexOfValue()"));
        }
    }

    // The mount gate above is `enabled`; the AUTO-grab gate is `enabled &&
    // autoGrab`. Keeping them distinct is the whole point — collapsing the
    // mount onto autoGrab would stop un-auto'd devices mounting (so they'd
    // never be configurable), and collapsing the grab onto `enabled` alone
    // would silently restore hands-off copying for every already-enabled rule.
    void usbGrabber_hotplugGrabRequiresAutoGrabNotJustEnabled()
    {
        // stripComments: this function's comments explain the two gates BY NAME,
        // so a raw scan would pass on the prose even if the code stopped
        // checking. functionBody() brace-matches rather than trusting a "\n}"
        // delimiter that any lambda or nested block would break.
        const QString src = stripComments(readAll(QStringLiteral(SUOP_SRC_DIR "/core/UsbGrabberController.cpp")));
        QVERIFY2(!src.isEmpty(), "src/core/UsbGrabberController.cpp must exist");

        // The gate moved into maybeAutoGrab() (2026-07: shared by hotplug
        // handleDeviceAdded AND the startup rescanForAutoGrab, so both apply it
        // identically). Scan that helper now — handleDeviceAdded is just a
        // one-line delegate to it.
        const QString handler = functionBody(src, QStringLiteral("void UsbGrabberController::maybeAutoGrab("));
        QVERIFY2(!handler.isEmpty(), "maybeAutoGrab must exist — it IS the shared auto-grab gate");

        QVERIFY2(handler.contains(QStringLiteral(".autoGrab")),
                 "maybeAutoGrab() must gate on rule.autoGrab as well as rule.enabled: a device may only "
                 "grab by itself when its owner opted that device into auto-grabbing");
        QVERIFY2(handler.contains(QStringLiteral(".enabled")),
                 "maybeAutoGrab() must still ALSO gate on rule.enabled — autoGrab is narrower than "
                 "enabled, never a way around it");
    }

    // A grab must stay a task on the shared queue. Reverting it to a thread of
    // its own would silently take away its Tasks row, the progress glow and
    // pause/cancel, and let it fight a running transcription for the disk.
    void usbGrabber_grabIsAQueuedTaskNotItsOwnThread()
    {
        // stripComments matters most for the two ABSENCE checks below: both
        // would fail the moment someone documented WHY the thing is absent.
        const QString src = stripComments(readAll(QStringLiteral(SUOP_SRC_DIR "/core/UsbGrabberController.cpp")));
        QVERIFY2(!src.isEmpty(), "src/core/UsbGrabberController.cpp must exist");

        const QString body = functionBody(src, QStringLiteral("void UsbGrabberController::startGrabJob("));
        QVERIFY2(!body.isEmpty(), "startGrabJob must still exist");

        QVERIFY2(body.contains(QStringLiteral("enqueueUsbGrab(")),
                 "startGrabJob() must enqueue the grab onto TranscriptionController (enqueueUsbGrab) — a grab "
                 "is a task in the one queue, not a bespoke thread");
        QVERIFY2(!body.contains(QStringLiteral("new UsbGrabPreviewJob")),
                 "startGrabJob() must not spawn a preview job: UsbGrabPreviewJob is the dry-run PREVIEW path "
                 "only (it deliberately skips the queue), and it copies nothing");

        // The preview, conversely, must NOT join the queue: it's an interactive
        // answer while editing a rule, and queueing it behind a transcription
        // would make the rule editor unusable.
        const QString preview = functionBody(src, QStringLiteral("void UsbGrabberController::requestGrabPreview("));
        QVERIFY2(!preview.isEmpty(), "requestGrabPreview must still exist");
        QVERIFY2(!preview.contains(QStringLiteral("enqueueUsbGrab(")),
                 "requestGrabPreview() must not go through the task queue — a preview writes nothing and is "
                 "wanted immediately, not after whatever is transcribing finishes");
    }

    // The auto-combiner reverses a decision that was deliberately made once
    // before (combine fired on every vault rebuild and was removed for silently
    // merging mid-recording). It's allowed back ONLY in this narrow shape:
    // opt-in, hung off the auto-grab barrier, and gated on a CLEAN batch — never
    // a vault-change hook. The orchestration lives in C++ (UsbGrabberController)
    // now, not Main.qml, so the safety-critical decision is unit-testable and the
    // clean-batch gate is enforced where it's computed; QML only announces it.
    void usbGrabber_autoCombineIsOptInAndOnlyAfterCleanAutoGrabsSettle()
    {
        // stripComments: this method's own comments legitimately NAME the calls
        // being checked (explaining why refresh() must precede allAudioPaths()),
        // so scanning raw source matches the prose instead of the code — and the
        // ordering check below then compares a comment against a call.
        const QString src = stripComments(readAll(QStringLiteral(SUOP_SRC_DIR "/core/UsbGrabberController.cpp")));
        QVERIFY2(!src.isEmpty(), "src/core/UsbGrabberController.cpp must exist");

        const QString body =
            functionBody(src, QStringLiteral("void UsbGrabberController::maybeAutoCombineAfterGrab("));
        QVERIFY2(!body.isEmpty(),
                 "UsbGrabberController must own the auto-combine (maybeAutoCombineAfterGrab) — moved out of "
                 "Main.qml so the safety-critical decision is testable in C++");

        QVERIFY2(body.contains(QStringLiteral("m_autoCombineAfterGrab")),
                 "the auto-combine must be gated on autoCombineAfterGrab (default off): combining moves the "
                 "user's original parts into .combined/, so it is opted into, never assumed");
        QVERIFY2(body.contains(QStringLiteral("runCombineScan(")),
                 "the auto-combine must go through the ordinary runCombineScan() — rule-driven and "
                 "'ready'-gated like every other combine path, not a bespoke merge");
        QVERIFY2(body.contains(QStringLiteral("->refresh()")),
                 "the auto-combine must refresh() the vault tree before scanning it: allAudioPaths() reads "
                 "the in-memory tree, which a filesystem WATCHER refreshes asynchronously (and never for "
                 "subfolders the grab just created) — scanning first would silently combine nothing");
        // Order matters as much as presence: refreshing after reading the paths
        // would be the same bug with extra steps.
        QVERIFY2(body.indexOf(QStringLiteral("->refresh()")) < body.indexOf(QStringLiteral("allAudioPaths()")),
                 "refresh() must come BEFORE allAudioPaths() — refreshing afterwards still scans the stale "
                 "tree");

        // The clean-batch gate is the whole point: the combine must be reachable
        // only through the barrier handler that checks m_autoBatchClean, never
        // called unconditionally on every settle.
        const QString settled =
            functionBody(src, QStringLiteral("void UsbGrabberController::onAutoGrabBatchSettled("));
        QVERIFY2(settled.contains(QStringLiteral("m_autoBatchClean"))
                     && settled.contains(QStringLiteral("maybeAutoCombineAfterGrab(")),
                 "onAutoGrabBatchSettled() must gate maybeAutoCombineAfterGrab() on m_autoBatchClean — an "
                 "aborted/errored grab may have left an incomplete part set, and combining it would drop the "
                 "recording's tail");

        // The reverted behavior, kept out: Main.qml must no longer drive the
        // combine at all (so it can't be re-hooked to a vault-tree reset either).
        const QString main = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml")));
        QVERIFY2(!main.contains(QStringLiteral("runCombineScan(")),
                 "Main.qml must no longer drive the combine directly — that orchestration moved into "
                 "UsbGrabberController so the clean-batch gate is enforced where it's computed");
    }

    // The preview panel survives the 2.5s device poll by design, so without an
    // explicit dismiss there is no way to put it away except previewing another
    // device or leaving the page.
    void usbGrabber_previewHasADismissControl()
    {
        // The USB grabber screen lives in UsbGrabberPage.qml (extracted from
        // MainPanel, 2026-07-19 decomposition).
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/UsbGrabberPage.qml")));
        QVERIFY2(!src.isEmpty(), "qml/UsbGrabberPage.qml must exist");

        QVERIFY2(src.contains(QStringLiteral("function dismissPreview(")),
                 "usbGrabberPage must expose dismissPreview() — the grab preview needs a way out");
        const int btnIdx = src.indexOf(QStringLiteral("id: dismissPreviewButton"));
        QVERIFY2(btnIdx >= 0, "the grab preview must have a dismiss button (id: dismissPreviewButton)");

        const QString btn = src.mid(btnIdx, 700);
        QVERIFY2(btn.contains(QStringLiteral("dismissPreview()")),
                 "the dismiss button must call dismissPreview()");
        // StyleGuide.md "Iconography" + "Interactable hover behavior".
        QVERIFY2(btn.contains(QStringLiteral("AppIcon")) && btn.contains(QStringLiteral("HoverBorder")),
                 "the dismiss button must be an AppIcon inside the standard HoverHandler/HoverBorder wrapper, "
                 "not a bare glyph");
    }

    // The Tasks popup (jobListPopup) reads as too tight when its symmetric inner
    // padding is small — the value feeds BOTH the ColumnLayout margin and the
    // popup's height calc. This is a floor, not an exact pin, so future taste
    // tweaks stay free; it just stops the popup silently regressing to cramped.
    void bottomBar_tasksPopupHasBreathingRoom()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/BottomBar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/BottomBar.qml must exist");

        // Comments are stripped, and "property int innerPadding" is unique to
        // jobListPopup, so a targeted match here can't land on a doc comment.
        const QRegularExpression innerPadding(
            QStringLiteral("property int innerPadding:\\s*(\\d+)"));
        const QRegularExpressionMatch m = innerPadding.match(src);
        QVERIFY2(m.hasMatch(), "jobListPopup must declare an innerPadding");
        QVERIFY2(m.captured(1).toInt() >= 12,
                 "the Tasks popup's inner padding must stay >= 12 so it doesn't read as cramped");
    }

    // The primary rename control (titleField) must be transcription-gated: in
    // non-labels mode it renames the real file, which must not race a running
    // transcription (see serialActiveConflictsWith / isSameRecording). Mirrors
    // the delete trio's busyOrQueuedUrls gate. Queue item 015.
    void mainPanel_titleFieldIsTranscriptionGated()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");
        const QString block = enclosingBlock(src, QStringLiteral("id: titleField"));
        QVERIFY2(!block.isEmpty(), "titleField must exist in MainPanel.qml");
        QVERIFY2(block.contains(QStringLiteral("enabled:")) && block.contains(QStringLiteral("busyOrQueuedUrls")),
                 "titleField must carry an enabled: binding referencing busyOrQueuedUrls so a rename "
                 "can't race a running transcription (queue item 015)");
    }

    // Chapter titles in the TOC are always-editable TextFields (StyleGuide.md
    // "Always-editable text"), NOT read-only TextEdits armed on tap — so a
    // fresh drag selects immediately in any chapter, with no positionAt hack.
    // Queue item 025.
    void mainPanel_tocChapterTitleIsAlwaysEditable()
    {
        // mainPanelSrc(): tocChapterTitle lives in ChapterTocList.qml now.
        const QString src = stripComments(mainPanelSrc());
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");
        const QString block = enclosingBlock(src, QStringLiteral("id: tocChapterTitle"));
        QVERIFY2(!block.isEmpty(), "id: tocChapterTitle must exist in MainPanel.qml");
        QVERIFY2(block.contains(QStringLiteral("background: UnderlineField")),
                 "the TOC chapter title must use the UnderlineField background (always-editable pattern)");
        QVERIFY2(block.contains(QStringLiteral("plainUntilHovered: true")),
                 "the TOC chapter title must opt out of the resting underline (plainUntilHovered)");
        QVERIFY2(!block.contains(QStringLiteral("readOnly: !")),
                 "the TOC chapter title must not be a read-only-until-armed TextEdit anymore");
        QVERIFY2(!block.contains(QStringLiteral("positionAt(")),
                 "the TOC chapter title must not hand-place the caret — the field owns its full width");
        // The StyleGuide bullet this pattern is now the documented default for,
        // including the carve-out that keeps the transcript heading as-is.
        const QString guide = readAll(QStringLiteral(SUOP_REPO_DIR "/StyleGuide.md"));
        QVERIFY2(guide.contains(QStringLiteral("This is the default for any single-line editable label")),
                 "StyleGuide.md must document always-editable as the default for single-line editable labels");
        QVERIFY2(guide.contains(QStringLiteral("Carve-out:")),
                 "StyleGuide.md must document the transcript-heading carve-out");
    }

    // Queue item 021: a structural task (rename/move/import/combine/grab) gets
    // its authoritative tree reflow from the vault watcher's modelReset, so
    // Sidebar.onFinished must NOT also re-sort — running both reflowed the left
    // panel twice for one rename. The per-row refreshFile() nudge stays
    // unconditional (an in-place edit has no other trigger).
    void sidebar_onFinishedSkipsReflowForStructuralTasks()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Sidebar.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Sidebar.qml must exist");
        const QString body = functionBody(src, QStringLiteral("function onFinished("));
        QVERIFY2(!body.isEmpty(), "Sidebar must have an onFinished handler");
        QVERIFY2(src.contains(QStringLiteral("function onFinished(fileUrl, ok, error, structural)")),
                 "Sidebar.onFinished must take the structural flag the finished() signal now carries");
        QVERIFY2(body.contains(QStringLiteral("if (!structural)")),
                 "Sidebar.onFinished must gate refreshTreeOrdering/refreshSearchResults behind "
                 "!structural — the watcher's modelReset already reflows a rename/move (item 021)");
        QVERIFY2(body.contains(QStringLiteral("refreshFile")),
                 "the per-row refreshFile() nudge must stay UNCONDITIONAL — an in-place edit "
                 "(label/tags/chapters) has no watcher event of its own");
    }

    // Queue item 023: a failed rename must raise ONE error snackbar, not two.
    // The error snackbar is owned by Sidebar.onFileRenamed (fires for every
    // rename); MainPanel.onFileRenamed must NOT also call Notifications.error,
    // and must clear renameInFlight so a second commit can't double-rename.
    void mainPanel_onFileRenamedDoesNotDoubleReportError()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");
        const QString body = functionBody(src, QStringLiteral("function onFileRenamed"));
        QVERIFY2(!body.isEmpty(), "MainPanel must have an onFileRenamed handler");
        QVERIFY2(!body.contains(QStringLiteral("Notifications.error")),
                 "MainPanel.onFileRenamed must not raise the error snackbar — Sidebar owns it "
                 "(otherwise one failed rename shows two snackbars, queue item 023)");
        QVERIFY2(body.contains(QStringLiteral("renameInFlight = false")),
                 "MainPanel.onFileRenamed must clear renameInFlight so a blur can't fire a second rename");
    }

    // Queue item 080: vadError (VAD couldn't run — model missing/offline) is a
    // durable per-file frontmatter field, same graceful-degradation shape as
    // speakerDetectionError, but had no UI surface — only visible by opening the
    // raw .md and reading the YAML. MainPanel must render it as the identical
    // inline warning banner. stripComments() first: the banner carries a doc
    // comment that names vadError, so a raw scan would match the comment and
    // silently stop guarding.
    void mainPanelSurfacesVadFallbackWarning()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");
        QVERIFY2(src.contains(QStringLiteral("!!mainPanel.metadata.vadError")),
                 "MainPanel must gate a banner's visibility on !!mainPanel.metadata.vadError, "
                 "mirroring the speakerDetectionError banner (queue item 080)");
        QVERIFY2(src.contains(QStringLiteral("mainPanel.metadata.vadError || \"\"")),
                 "MainPanel's vadError banner must bind the reason string via "
                 "mainPanel.metadata.vadError || \"\" (queue item 080)");
    }

    // Clicking empty space anywhere in the window must blur the focused text
    // field (committing it) — an app-wide TapHandler on rootContent that calls
    // forceActiveFocus() pulls active focus off any field. Source-scan (Main.qml
    // root has no QQuickView unit path). Queue item 022.
    void mainWindow_emptySpaceClickBlursFocusedField()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml")));
        QVERIFY2(!src.isEmpty(), "qml/Main.qml must exist");
        const QString block = enclosingBlock(src, QStringLiteral("id: rootContent"));
        QVERIFY2(!block.isEmpty(), "rootContent must exist in Main.qml");
        QVERIFY2(block.contains(QStringLiteral("TapHandler")) && block.contains(QStringLiteral("forceActiveFocus()")),
                 "rootContent must carry an app-wide TapHandler that forceActiveFocus()es to blur "
                 "the focused field on an empty-space click (queue item 022)");
    }

    // Queue item 072: click-outside-to-blur is made UNIVERSAL by a C++
    // application-level event filter (FocusBlurFilter) installed on the app, so
    // it observes every press before any QML grab (a ScrollView/Flickable eating
    // the press is why the QML root TapHandlers weren't universal). Two things a
    // future edit must not quietly undo: (1) main.cpp actually installs it, and
    // (2) the filter gates on the GENERIC editable-text-primitive check
    // (QQuickTextInput/QQuickTextEdit), never a hardcoded field id — that
    // genericity is what closes the "only two fields blur" completeness gap.
    void focusBlurFilter_installedAndGeneric()
    {
        const QString mainCpp = readAll(QStringLiteral(SUOP_SRC_DIR "/main.cpp"));
        QVERIFY2(!mainCpp.isEmpty(), "src/main.cpp must exist");
        const QString mainNoComments = stripComments(mainCpp);
        QVERIFY2(mainNoComments.contains(QStringLiteral("FocusBlurFilter"))
                     && mainNoComments.contains(QStringLiteral("installEventFilter")),
                 "main.cpp must construct a FocusBlurFilter and installEventFilter() it on the app "
                 "(queue item 072 — the app-wide click-outside-to-blur)");

        const QString filter =
            stripComments(readAll(QStringLiteral(SUOP_SRC_DIR "/core/FocusBlurFilter.cpp")));
        QVERIFY2(!filter.isEmpty(), "src/core/FocusBlurFilter.cpp must exist");
        const QString body =
            functionBody(filter, QStringLiteral("bool FocusBlurFilter::eventFilter"));
        QVERIFY2(!body.isEmpty(), "FocusBlurFilter::eventFilter must exist");
        QVERIFY2(body.contains(QStringLiteral("QQuickTextInput"))
                     && body.contains(QStringLiteral("QQuickTextEdit")),
                 "FocusBlurFilter must gate on the generic editable-text-primitive check "
                 "(QQuickTextInput/QQuickTextEdit), not a hardcoded field id — so the blur stays "
                 "universal and never re-narrows to title-only (queue item 072)");
    }

    // The read-only "Previously transcribed …" info line must stay focusable
    // (activeFocusOnPress not false) so a keyboard Copy (Ctrl+C) reaches it —
    // selectByMouse alone highlights the text without focus, and StandardKey.Copy
    // is a key event that only lands on the focused item.
    void previouslyTranscribedInfoLineIsCopyable()
    {
        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");
        const QString block = enclosingBlock(src, QStringLiteral("previouslyTranscribedInfo"));
        QVERIFY2(!block.isEmpty(), "the Previously-transcribed info line must exist in MainPanel.qml");
        QVERIFY2(block.contains(QStringLiteral("selectByKeyboard: true")),
                 "the info line must be keyboard-selectable");
        QVERIFY2(!block.contains(QStringLiteral("activeFocusOnPress: false")),
                 "the Previously-transcribed info line must stay focusable (activeFocusOnPress not false) "
                 "so Ctrl+C can reach it — see queue item 013");
    }

    // The no-callback phases (loading/decoding/detecting-speech) get their
    // per-phase fraction synthesized + timer-driven. That phase set once drifted
    // (synthesized for detecting-speech but only ran the timer for
    // loading/decoding, freezing that phase's bar) — so both the synthetic-
    // fraction branch and the timer-start gate must route through the single
    // isSyntheticProgressPhase predicate, never a hand-rolled phase list.
    void syntheticProgressPhaseSet_hasOneSourceOfTruth()
    {
        // 2026-07-20: the predicate and the synthetic-fraction branch moved to
        // PhaseTimer with the rest of the phase-timing subsystem; the
        // timer-start gate stayed on the controller (it drives a controller-owned
        // QTimer). So this guard now spans both files — the point is unchanged:
        // ONE predicate, two consumers.
        const QString timer = stripComments(
            readAll(QStringLiteral(SUOP_SRC_DIR "/core/PhaseTimer.cpp")));
        QVERIFY2(!timer.isEmpty(), "PhaseTimer.cpp must exist");
        const QString src = stripComments(
            readAll(QStringLiteral(SUOP_SRC_DIR "/core/TranscriptionController.cpp")));
        QVERIFY2(!src.isEmpty(), "TranscriptionController.cpp must exist");

        // The predicate must exist and name all three no-callback phases.
        const QString body = functionBody(timer,
            QStringLiteral("bool PhaseTimer::isSyntheticProgressPhase(const QString &phase)"));
        QVERIFY2(!body.isEmpty(), "isSyntheticProgressPhase() must exist");
        for (const char *p : {"loading", "decoding", "detecting-speech"})
            QVERIFY2(body.contains(QLatin1String(p)),
                     QByteArrayLiteral("isSyntheticProgressPhase must list ").append(p));

        // Both consumers must go through the predicate — no hand-rolled phase
        // list that could omit one (that drift is exactly what froze detecting-speech).
        const QString synth = functionBody(timer,
            QStringLiteral("double PhaseTimer::phaseRelativeSyntheticProgress(const QString &phase"));
        QVERIFY2(synth.contains(QStringLiteral("isSyntheticProgressPhase")),
                 "phaseRelativeSyntheticProgress() must gate on isSyntheticProgressPhase, not a literal phase list");
        // The gate may OR-in the SelfCumulative frozen-bar fallback
        // (selfCumulativeNeedsSynthetic), so match the predicate call itself
        // rather than the exact `if (...)` wrapper — the invariant is that the
        // WeightedPhases half of the gate still routes through the one predicate,
        // never a hand-rolled literal phase list.
        QVERIFY2(src.contains(QStringLiteral("PhaseTimer::isSyntheticProgressPhase(phase)")),
                 "the m_indeterminatePhaseTimer start gate must use isSyntheticProgressPhase, not a literal phase list");
    }

    // Top-level full-page screens must scroll via the shared ScrollPage base
    // (StyleGuide.md "Top-level pages scroll via ScrollPage") — the edge-gutter
    // scrollbar and middle-click autoscroll come from the base, so a page
    // hand-rolling its own page-level ScrollView silently loses both (that was
    // exactly the benchmark/USB/recorder defect this base fixed). Source-scan:
    // page chrome geometry, no QQuickView harness.
    void topLevelScrollPages_useScrollPage()
    {
        // The shared payload itself can't silently regress.
        const QString page = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/ScrollPage.qml")));
        QVERIFY2(!page.isEmpty(), "qml/ScrollPage.qml must exist");
        QVERIFY2(page.contains(QStringLiteral("MiddleDragScroll")),
                 "ScrollPage must bake in Chrome-style middle-click autoscroll");
        QVERIFY2(page.contains(QStringLiteral("scrollBarGutter")),
                 "ScrollPage must own the edge-gutter scrollbar geometry");

        const QString src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!src.isEmpty(), "qml/MainPanel.qml must exist");

        // The QML type declared just before the block holding `anchor`: the
        // identifier immediately preceding that block's opening brace.
        // enclosingBlock() can't answer this — its slice starts AT the '{', so
        // the type name sits just outside what it returns.
        const auto declaredType = [&src](const QString &anchor) -> QString {
            const int anchorIdx = src.indexOf(anchor);
            if (anchorIdx < 0)
                return QString();
            const int open = src.lastIndexOf(QLatin1Char('{'), anchorIdx);
            if (open < 0)
                return QString();
            int end = open - 1;
            while (end >= 0 && src.at(end).isSpace())
                --end;
            int begin = end;
            while (begin >= 0 && (src.at(begin).isLetterOrNumber() || src.at(begin) == QLatin1Char('.')))
                --begin;
            return src.mid(begin + 1, end - begin);
        };

        QCOMPARE(declaredType(QStringLiteral("id: notePage")), QStringLiteral("ScrollPage"));
        QCOMPARE(declaredType(QStringLiteral("id: benchmarkScroll")), QStringLiteral("ScrollPage"));

        // The recorder screen moved into its own RecorderPage.qml component
        // (item 069), taking its recorderScroll ScrollPage with it.
        const QString recPage = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/RecorderPage.qml")));
        QVERIFY2(!recPage.isEmpty(), "qml/RecorderPage.qml must exist");
        const auto recDeclaredType = [&recPage](const QString &anchor) -> QString {
            const int anchorIdx = recPage.indexOf(anchor);
            if (anchorIdx < 0)
                return QString();
            const int open = recPage.lastIndexOf(QLatin1Char('{'), anchorIdx);
            if (open < 0)
                return QString();
            int end = open - 1;
            while (end >= 0 && recPage.at(end).isSpace())
                --end;
            int begin = end;
            while (begin >= 0 && (recPage.at(begin).isLetterOrNumber() || recPage.at(begin) == QLatin1Char('.')))
                --begin;
            return recPage.mid(begin + 1, end - begin);
        };
        QCOMPARE(recDeclaredType(QStringLiteral("id: recorderScroll")), QStringLiteral("ScrollPage"));
        // The recorder's rows must live INSIDE its ScrollPage so a short
        // window scrolls instead of clipping them with no recourse.
        QVERIFY2(enclosingBlock(recPage, QStringLiteral("id: recorderScroll"))
                     .contains(QStringLiteral("id: recorderContent")),
                 "the recorder's content column must live inside its ScrollPage");

        // The USB grabber screen moved into its own UsbGrabberPage.qml
        // component (item 068), taking its usbScrollArea ScrollPage with it.
        const QString usbPage = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/UsbGrabberPage.qml")));
        QVERIFY2(!usbPage.isEmpty(), "qml/UsbGrabberPage.qml must exist");
        const auto usbDeclaredType = [&usbPage](const QString &anchor) -> QString {
            const int anchorIdx = usbPage.indexOf(anchor);
            if (anchorIdx < 0)
                return QString();
            const int open = usbPage.lastIndexOf(QLatin1Char('{'), anchorIdx);
            if (open < 0)
                return QString();
            int end = open - 1;
            while (end >= 0 && usbPage.at(end).isSpace())
                --end;
            int begin = end;
            while (begin >= 0 && (usbPage.at(begin).isLetterOrNumber() || usbPage.at(begin) == QLatin1Char('.')))
                --begin;
            return usbPage.mid(begin + 1, end - begin);
        };
        QCOMPARE(usbDeclaredType(QStringLiteral("id: usbScrollArea")), QStringLiteral("ScrollPage"));

        // Guards against re-hand-rolling the old chrome: the base owns the
        // page's 16px margins, so none of these blocks may re-declare them
        // (the old bare ScrollView + anchors.margins: 16 shape).
        {
            const QString block = enclosingBlock(src, QStringLiteral("id: benchmarkScroll"));
            QVERIFY2(!block.isEmpty(), "MainPanel.qml must declare id: benchmarkScroll");
            QVERIFY2(!block.contains(QStringLiteral("anchors.margins: 16")),
                     "ScrollPage owns the page margins — drop anchors.margins from the benchmark block");
        }
        const QString recContentBlock = enclosingBlock(recPage, QStringLiteral("id: recorderContent"));
        QVERIFY2(!recContentBlock.isEmpty(), "RecorderPage.qml must declare id: recorderContent");
        QVERIFY2(!recContentBlock.contains(QStringLiteral("anchors.margins: 16")),
                 "ScrollPage owns the page margins — drop anchors.margins from the recorder content block");
        const QString usbBlock = enclosingBlock(usbPage, QStringLiteral("id: usbScrollArea"));
        QVERIFY2(!usbBlock.isEmpty(), "UsbGrabberPage.qml must declare id: usbScrollArea");
        QVERIFY2(!usbBlock.contains(QStringLiteral("anchors.margins: 16")),
                 "ScrollPage owns the page margins — drop anchors.margins from the USB scroll block");
    }

    // Item 067: the transcript note page is a ScrollPage instance like every
    // other top-level screen — the sticky rows in its header slot, the
    // transcript body as its single content child, the transcribing spinner in
    // its bodyOverlay. This is the RISKY port (live-scroll auto-follow,
    // sentence-seek math, and the QQuickView/offscreen harnesses that look the
    // inner ScrollView up by objectName), so guard the load-bearing wiring:
    // ScrollPage must expose the slots the port consumes, the note page must
    // reach the ScrollView through the alias (not a dead id), and the harness
    // objectName must survive.
    void notePage_usesScrollPage()
    {
        const QString stripped = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml")));
        QVERIFY2(!stripped.isEmpty(), "qml/MainPanel.qml must exist");

        // (a) the note page IS a ScrollPage with id: notePage.
        const int idIdx = stripped.indexOf(QStringLiteral("id: notePage"));
        QVERIFY2(idIdx >= 0, "the note page must declare id: notePage");
        const int open = stripped.lastIndexOf(QLatin1Char('{'), idIdx);
        QVERIFY2(open >= 0, "id: notePage must sit inside a block");
        int end = open - 1;
        while (end >= 0 && stripped.at(end).isSpace())
            --end;
        int begin = end;
        while (begin >= 0 && (stripped.at(begin).isLetterOrNumber() || stripped.at(begin) == QLatin1Char('.')))
            --begin;
        QCOMPARE(stripped.mid(begin + 1, end - begin), QStringLiteral("ScrollPage"));

        // (b) harness contract: the inner ScrollView still answers to objectName
        // "transcriptScrollView", passed through ScrollPage's bodyObjectName alias.
        QVERIFY2(stripped.contains(QStringLiteral("bodyObjectName: \"transcriptScrollView\"")),
                 "the note page must keep the transcriptScrollView objectName (the QQuickView/"
                 "offscreen harness contract) via ScrollPage's bodyObjectName alias");

        // (c) live-scroll auto-follow + sentence-seek math must reach the inner
        // ScrollView through notePage.bodyScrollView, with no dead id left over.
        QVERIFY2(stripped.contains(QStringLiteral("notePage.bodyScrollView.contentItem")),
                 "auto-follow/seek math must reach the ScrollView via notePage.bodyScrollView.contentItem");
        QVERIFY2(!stripped.contains(QStringLiteral("transcriptScrollView.contentItem")),
                 "no dangling transcriptScrollView id reference may remain after the ScrollPage port");

        // (d) ScrollPage exposes the slots this port depends on.
        const QString page = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/ScrollPage.qml")));
        for (const char *alias : {"property alias header:", "property alias bodyOverlay:",
                                  "property alias bodyScrollView:", "property alias bodyObjectName:"}) {
            QVERIFY2(page.contains(QLatin1String(alias)),
                     QByteArrayLiteral("ScrollPage must expose ").append(alias));
        }
    }

};

QTEST_GUILESS_MAIN(TstStyleGuide)
#include "tst_styleguide.moc"
