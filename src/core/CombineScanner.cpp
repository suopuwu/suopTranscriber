#include "CombineScanner.h"

#include "CombineNaming.h"
#include "RegexValidation.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QVariantMap>

#include <algorithm>
#include <optional>
#include <tuple>

// findCombineGroups()'s "pending" status: a group is otherwise "ready" but
// its newest member was modified more recently than this many seconds ago —
// the recorder may still be actively writing that file, and concatenating a
// growing/not-yet-closed file risks truncation or a corrupt merge.
constexpr qint64 kCombineStabilityWindowSecs = 5;

// See this method's header doc comment for the full matching/grouping/status
// semantics.
QList<CombineGroup> CombineScanner::findCombineGroups(const QVariantList &rules, const QStringList &audioPaths)
{
    struct Candidate {
        QString path;
        QString ruleName;
        // The containing folder, part of the group identity below. A rule
        // matches on bare filename, so without this two same-named parts in
        // different folders would group together — which is never what the
        // user means (they're separate recordings that happen to share a
        // naming scheme), and the tree can't draw one outline around rows
        // that aren't siblings anyway.
        QString dir;
        QString key;
        // Same capture groups as `key`, un-concatenated — every member of a
        // group shares identical values here by construction (that's what
        // makes them a group), so the group only needs one member's copy.
        QStringList capturedValues;
        // The rule's designated sort differentiator's captured text, if the
        // rule has one AND it participated in this match — see the sort
        // logic below for how this overrides the mtime default. A rule with
        // no sort differentiator configured (the common case) leaves every
        // candidate's sortValue empty (std::nullopt).
        std::optional<QString> sortValue;
        // Default combine-order signal, and always the stability-window
        // signal below regardless of sort order: deliberately mtime, NOT
        // file creation/birth time. Creation time would seem more semantically
        // "correct" (it's when the recorder started this part), but it
        // doesn't survive a plain file copy on Linux — cp/rsync/a
        // cross-filesystem mv all stamp a fresh birth time on the
        // destination, while mtime is preserved by every standard copy tool.
        // Since this app doesn't control (and shouldn't assume anything
        // about) how audio lands in the vault — direct recording onto a
        // mounted vault path vs. copied off an SD card vs. synced some other
        // way — mtime is the only signal that's reliably correct across all
        // of them.
        QDateTime mtime;
    };

    QList<Candidate> candidates;
    for (const QVariant &ruleVar : rules) {
        const QVariantMap rule = ruleVar.toMap();
        const QString ruleName = rule.value(QStringLiteral("name")).toString();
        for (const QString &path : audioPaths) {
            const QFileInfo info(path);
            QStringList capturedValues;
            std::optional<QString> sortValue;
            if (const auto key = matchRuleCapturedKey(rule, info.fileName(), &capturedValues, &sortValue))
                candidates.append(Candidate{path, ruleName, info.absolutePath(), *key, capturedValues,
                                            sortValue, info.lastModified()});
        }
    }

    QMap<std::tuple<QString, QString, QString>, QList<Candidate>> buckets;
    for (const Candidate &c : candidates)
        buckets[std::make_tuple(c.ruleName, c.dir, c.key)].append(c);

    const QDateTime now = QDateTime::currentDateTime();
    QList<CombineGroup> groups;
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it) {
        QList<Candidate> members = it.value();

        // Combine order: the rule's sort differentiator (numeric-aware if
        // every member's captured value parses as a number, else plain
        // string comparison), if configured and every member actually
        // captured a value for it; otherwise mtime ascending (see
        // Candidate::mtime's doc comment for why mtime, not creation time).
        // "Numeric" is decided once for the whole group, not per pair — a
        // comparator that's numeric for some pairs and lexicographic for
        // others isn't a strict weak ordering, which is undefined behavior
        // for std::sort.
        const bool allHaveSortValue = std::all_of(members.begin(), members.end(),
                                                   [](const Candidate &c) { return c.sortValue.has_value(); });
        if (allHaveSortValue) {
            const bool allNumeric = std::all_of(members.begin(), members.end(), [](const Candidate &c) {
                bool ok = false;
                c.sortValue->toDouble(&ok);
                return ok;
            });
            if (allNumeric) {
                std::sort(members.begin(), members.end(), [](const Candidate &a, const Candidate &b) {
                    return a.sortValue->toDouble() < b.sortValue->toDouble();
                });
            } else {
                std::sort(members.begin(), members.end(),
                          [](const Candidate &a, const Candidate &b) { return *a.sortValue < *b.sortValue; });
            }
        } else {
            std::sort(members.begin(), members.end(),
                      [](const Candidate &a, const Candidate &b) { return a.mtime < b.mtime; });
        }

        CombineGroup group;
        group.ruleName = std::get<0>(it.key());
        group.key = std::get<2>(it.key());
        group.capturedValues = members.first().capturedValues;
        for (const Candidate &c : members)
            group.files << c.path;

        // Every member shares the bucket's folder by construction, so the
        // merged file lands beside its own inputs rather than in whichever
        // folder happened to hold the first-in-combine-order member.
        group.outputPath = combineOutputPathFor(group.ruleName, group.key, std::get<1>(it.key()),
                                                QFileInfo(members.first().path).suffix());

        if (members.size() < 2) {
            group.status = QStringLiteral("singleton");
        } else if (QFileInfo::exists(group.outputPath)) {
            group.status = QStringLiteral("alreadyCombined");
        } else {
            bool anyTranscribed = false;
            for (const Candidate &c : members) {
                if (QFileInfo::exists(c.path + QStringLiteral(".md"))) {
                    anyTranscribed = true;
                    break;
                }
            }
            // Independent of whatever order `members` ended up sorted into
            // above (recording-order sort ≠ mtime order once a sort
            // differentiator is configured) — this check is genuinely about
            // "is the recorder still actively writing the newest part," so
            // it always needs the TRUE newest mtime, not just members.last().
            const auto newest = std::max_element(members.begin(), members.end(),
                                                  [](const Candidate &a, const Candidate &b) { return a.mtime < b.mtime; });
            // Deliberately NO completeness/contiguity check on the sort
            // differentiator (e.g. "the numbers 1,2,4 have a gap, so part 3 is
            // missing"). The differentiator is an ORDERING key, not a part
            // counter: a group of PART1/PART2/PART10 is legitimate and complete
            // (see tst_filecombine's sortDifferentiator_numericOverridesMtimeOrder),
            // and nothing on disk can tell "take 10 after take 2, no 3–9" apart
            // from "parts 3–9 were lost". The one incompleteness that DOES have a
            // reliable signal — a grab that aborted mid-copy, which can truncate
            // the TAIL of a set a gap check couldn't catch anyway — is handled
            // upstream: UsbGrabberController only auto-combines after a CLEAN grab
            // batch (see m_autoBatchClean). So this stays a pure grouping/mtime
            // decision; don't re-add a gap heuristic here (it only produces false
            // "incomplete"s).
            if (anyTranscribed)
                group.status = QStringLiteral("alreadyTranscribed");
            else if (newest->mtime.secsTo(now) < kCombineStabilityWindowSecs)
                group.status = QStringLiteral("pending");
            else
                group.status = QStringLiteral("ready");
        }
        groups.append(group);
    }

    // Deterministic ordering — callers (tests, the Settings preview) don't
    // have to guess QMap's iteration order.
    // outputPath is the tiebreaker, not decoration: (ruleName, key) alone
    // stopped being unique once folder joined the group identity — the same
    // rule and key in two folders are now two distinct groups.
    std::sort(groups.begin(), groups.end(), [](const CombineGroup &a, const CombineGroup &b) {
        if (a.ruleName != b.ruleName)
            return a.ruleName < b.ruleName;
        if (a.key != b.key)
            return a.key < b.key;
        return a.outputPath < b.outputPath;
    });
    return groups;
}

QVariantList CombineScanner::detectCombineGroups(const QVariantList &rules, const QStringList &audioPaths)
{
    QVariantList result;
    for (const CombineGroup &group : findCombineGroups(rules, audioPaths)) {
        QVariantMap entry;
        entry[QStringLiteral("ruleName")] = group.ruleName;
        entry[QStringLiteral("key")] = group.key;
        entry[QStringLiteral("capturedValues")] = group.capturedValues;
        entry[QStringLiteral("files")] = group.files;
        entry[QStringLiteral("outputPath")] = group.outputPath;
        entry[QStringLiteral("status")] = group.status;
        result << entry;
    }
    return result;
}

QString CombineScanner::regexValidationError(const QString &pattern)
{
    return regexValidationErrorFor(pattern);
}

QStringList CombineScanner::differentiatorNonParticipatingFiles(const QString &pattern, int groupIndex,
                                                                const QStringList &audioPaths)
{
    QStringList offending;
    const QRegularExpression re(pattern);
    if (!re.isValid() || groupIndex < 1 || groupIndex > re.captureCount())
        return offending;
    for (const QString &path : audioPaths) {
        const QFileInfo info(path);
        const QRegularExpressionMatch m = re.match(info.fileName());
        if (m.hasMatch() && m.capturedStart(groupIndex) < 0)
            offending << info.fileName();
    }
    return offending;
}
