#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>

// One detected group of split-recording parts — see
// CombineScanner::findCombineGroups()'s doc comment for the matching/grouping/
// status semantics. Mirrors CombineScanner::detectCombineGroups()'s
// QVariantMap shape one-to-one; kept as a real struct so the controller's
// queue glue (maybeRedirectForCombine()'s per-job safety check and
// enqueueReadyCombineGroups()) doesn't have to round-trip through QVariant.
struct CombineGroup {
    QString ruleName;
    QString key;
    // The rule's GROUPING capture groups, un-concatenated (key is these
    // joined with no separator) — deliberately excludes the sort
    // differentiator's own group, if the rule has one (see
    // findCombineGroups()'s doc comment: a sort differentiator is
    // explicitly not a grouping one). Exposed to detectCombineGroups()'s
    // QVariantList so the Settings preview can show a header of exactly
    // which differentiator values grouped these files, not just the
    // flattened key.
    QStringList capturedValues;
    QStringList files; // combine order — see findCombineGroups()'s doc comment
    QString outputPath;
    QString status; // "ready"/"singleton"/"alreadyTranscribed"/"alreadyCombined"/"pending"
};

// The split-recording combine DETECTION collaborator split out of
// TranscriptionController. It owns exactly one thing: turning
// (combine rules, audio paths) into the set of groups those rules describe,
// plus the two pattern-authoring helpers the Settings combine page needs to
// validate a rule against the SAME engine this detection actually uses.
//
// Everything here is pure over its inputs — the only disk it touches is
// QFileInfo stats on the paths handed in — which is what lets one entry point
// back BOTH the Settings "test this rule" live preview and the real enqueue
// decisions. It deliberately owns NO queue state: which groups have already
// been enqueued, the cached scan inputs a later per-job redirect re-checks
// against, and the actual task insertion all stay on the controller
// (enqueueCombineJob()/enqueueReadyCombineGroups()/maybeRedirectForCombine()),
// which is the only thing that can see the queue.
//
// Stateless by construction — never instantiated, hence the deleted
// constructor. The controller holds no member for it; it calls these
// statics directly, and its own Q_INVOKABLE detectCombineGroups()/
// regexValidationError()/differentiatorNonParticipatingFiles() remain on the
// controller as thin forwarders so QML's call sites are unchanged.
class CombineScanner
{
public:
    CombineScanner() = delete;

    // A group otherwise "ready" whose newest member was modified within this
    // many seconds of now reads as "pending" instead — the recorder may still
    // be writing that file, and concatenating a not-yet-closed file risks a
    // truncated/corrupt merge. Public so the sidebar's passive re-scan timer
    // (Sidebar.qml refreshTreeOrdering) can wait exactly this long for a
    // freshly-arrived group to settle, instead of hardcoding a parallel value.
    static constexpr qint64 kCombineStabilityWindowSecs = 5;

    // Matches every path in audioPaths against every enabled rule in `rules`
    // ({name, pattern, enabled, sortGroupIndex} maps) using QRegularExpression
    // SEARCH semantics (not anchored unless the pattern itself has ^/$)
    // against the full filename (name+ext). A match's capture groups
    // (captured(1..N), concatenated, EXCLUDING the sortGroupIndex one if the
    // rule has one — see below) are the group-identity "key", together with
    // the file's CONTAINING FOLDER: a rule only ever sees the bare filename,
    // so identically-named parts sitting in two different folders are two
    // separate groups, never one cross-folder group. They're unrelated
    // recordings that merely share a naming scheme, and the vault tree can't
    // draw one outline around rows that aren't siblings regardless. This also
    // makes outputPath unambiguous — the merged file always lands in the
    // group's own folder. An
    // uncaptured wildcard span (bare `.`, `\d{3}`, `.*`, ...) varies freely
    // between members without affecting grouping, matching the user's
    // stated model of "the rule gets the real string with the wildcard
    // replaced; same real string = combine". A rule with zero grouping
    // capture groups therefore groups EVERY file it matches into one bucket
    // (expected regex behavior, not a special case).
    //
    // Combine ORDER within a group: `sortGroupIndex` (1-based capture-group
    // number, -1/absent = none) optionally names ONE of the rule's capture
    // groups as the SORT differentiator instead of a grouping one — the
    // Settings UI enforces "at most one, and it stops being a grouping
    // differentiator the moment it's chosen" (matchRuleCapturedKey() excludes
    // it from `key`/capturedValues entirely). When every member of a group
    // captured a value for it, members are ordered by that value — compared
    // numerically (QString::toDouble()) if EVERY member's value parses as a
    // number, else compared as plain strings; "numeric or not" is decided
    // once for the whole group, not per pair, since a comparator that
    // switches between numeric and lexicographic per pair isn't a strict
    // weak ordering (undefined behavior for std::sort). Falls back to mtime
    // ascending — the default, and the ONLY order when no sort
    // differentiator is configured — deliberately NOT file creation/birth
    // time: birth time doesn't survive a plain file copy on Linux (cp/rsync/
    // a cross-filesystem mv all stamp a fresh one on the destination), while
    // mtime is preserved by every standard copy tool, and this app has no
    // way to know (or reason to assume) whether a given vault's audio was
    // recorded directly onto the vault path or copied in from an SD card —
    // mtime is the only signal that's correct either way.
    //
    // outputPath is derived from (sanitized ruleName, sanitized key) rather
    // than any single member's filename, so it's re-derivable from just one
    // surviving filename even after the rest have been moved into
    // .combined/ — see TranscriptionController::maybeRedirectForCombine()'s
    // safety-net re-check, which relies on exactly that. status: "singleton"
    // (only one match), "alreadyTranscribed"
    // (skip — some member already has a sibling .md, never silently discard
    // existing work), "alreadyCombined" (outputPath already exists on disk),
    // "pending" (would be ready, but the group's true most-recently-modified
    // member — independent of whatever order the group ended up sorted into
    // above — has an mtime inside kCombineStabilityWindowSecs of "now": the
    // recorder may still be writing it), else "ready".
    static QList<CombineGroup> findCombineGroups(const QVariantList &rules, const QStringList &audioPaths);

    // Split-recording combine: matches vault audio filenames against
    // user-authored regex rules (AppSettings.combineRules) to find groups of
    // files that are really one recording split by the recorder — see
    // findCombineGroups() above for the full matching/grouping
    // semantics. Pure over its inputs (only touches disk via QFileInfo stats
    // on the paths passed in), so this same entry point backs both the
    // Settings "test this rule" preview AND runCombineScan()'s real enqueue
    // decisions. Each rule map: {name, pattern, enabled, sortGroupIndex}.
    // Returns one entry per detected group: {ruleName, key, capturedValues
    // (QStringList, the rule's GROUPING capture groups un-concatenated — key
    // is these joined with no separator), files (QStringList, in combine
    // order — see findCombineGroups()'s doc comment for the sort
    // differentiator / mtime-default ordering rules), outputPath, status
    // ("ready"/"singleton"/"alreadyTranscribed"/"alreadyCombined"/"pending")}.
    static QVariantList detectCombineGroups(const QVariantList &rules, const QStringList &audioPaths);

    // Live validity check for a combine-rule pattern as the user types it in
    // Settings — validates against the EXACT SAME regex engine
    // findCombineGroups()/matchRuleCapturedKey() actually use (QRegularExpression),
    // not QML/JS RegExp, whose syntax isn't guaranteed identical. Empty pattern
    // is not an error (that's just "no rule authored yet"); returns the empty
    // string on success, QRegularExpression's own errorString() otherwise.
    static QString regexValidationError(const QString &pattern);

    // Backs Settings' per-rule "differentiator" buttons (SettingsPopup.qml):
    // a differentiator is just a capturing group whose captured text joins
    // matchRuleCapturedKey()'s combine-grouping key, but ONLY if it
    // actually participates in every match — a capturing group sitting in
    // an alternation branch that isn't always taken (e.g.
    // `(sometext)|(some other text)`) doesn't reliably differentiate
    // anything for the files that took the OTHER branch. Returns the
    // subset of audioPaths where `pattern` (already the CANDIDATE pattern
    // text — this method does no wrapping/rewriting itself) matches the
    // filename overall but capture group `groupIndex` (1-based) has
    // QRegularExpressionMatch::capturedStart() < 0, i.e. did not
    // participate in that match at all. A valid-but-zero-length
    // participation (e.g. `\d*` matching "") is NOT flagged — only
    // non-participation is. Empty result = safe to use as a differentiator
    // across every file currently in the vault.
    static QStringList differentiatorNonParticipatingFiles(const QString &pattern, int groupIndex,
                                                           const QStringList &audioPaths);
};
