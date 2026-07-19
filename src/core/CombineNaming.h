#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

// The ONE source of truth for a split-recording group's merged-output filename.
// Header-only + inline on purpose: both the combiner
// (TranscriptionController.cpp — findCombineGroups(), maybeRedirectForCombine())
// and the USB grabber (UsbGrabJob.cpp — the already-combined skip) must derive
// byte-identical names, or the grabber can't recognise a group the combiner
// already merged. Two translation units call these, so `inline` avoids a
// duplicate-symbol link error without a separate .cpp to register in every
// test target's SOURCES list.

// combineOutputPathFor()'s name is built from (ruleName, capturedKey), neither
// of which is guaranteed filesystem-safe — a regex capture group can pull out
// literally anything. Collapse every run of unsafe characters to a single
// SPACE (spaces are allowed, for readability) so the derived name is always a
// valid, human-readable single path segment on every filesystem this app
// targets, then trim leading/trailing separator noise so a name never opens or
// closes on a space or dash.
inline QString sanitizeForFilename(const QString &raw)
{
    QString out = raw;
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9 _-]+"));
    out.replace(unsafe, QStringLiteral(" "));
    static const QRegularExpression runs(QStringLiteral("  +"));
    out.replace(runs, QStringLiteral(" "));
    out = out.trimmed();
    while (out.startsWith(QChar('-')))
        out.remove(0, 1);
    while (out.endsWith(QChar('-')))
        out.chop(1);
    out = out.trimmed();
    return out.isEmpty() ? QStringLiteral("combined") : out;
}

// One rule ({name, pattern, enabled, sortGroupIndex} map) matched against a
// single filename (not a full path — QFileInfo::fileName()), search
// semantics (QRegularExpression, unanchored unless the pattern itself has
// ^/$). On a match, returns the "real string" key: capture groups 1..N joined
// with a SPACE in order, EXCLUDING the rule's sort differentiator group if it
// has one, skipping empty (non-participating optional) groups, and collapsing a
// capture that's identical to the one just before it — so a rule that captures
// the same token twice yields "20260710 edit", not "20260710 edit edit". Empty
// key if the pattern has no OTHER capture groups — see
// TranscriptionController::findCombineGroups()'s doc comment for the full
// grouping semantics. Shared by findCombineGroups() (which tries every rule
// against every candidate path), maybeRedirectForCombine() and the USB
// grabber's already-combined skip (both of which try every rule against ONE
// already-known filename). `capturedValuesOut`, if given, is filled with the
// SAME (non-sort, non-empty, dedup'd) capture values un-joined — only
// findCombineGroups() needs the individual values (for the Settings preview
// header showing which differentiator values grouped these files).
// `sortValueOut`, if given, receives the sort differentiator group's own
// captured text (untouched/nullopt if the rule has none configured, or the
// configured index is out of range for this match) — only findCombineGroups()
// uses this, to order a group's members by that value instead of the mtime
// default.
inline std::optional<QString> matchRuleCapturedKey(const QVariantMap &rule, const QString &fileName,
                                                    QStringList *capturedValuesOut = nullptr,
                                                    std::optional<QString> *sortValueOut = nullptr)
{
    if (!rule.value(QStringLiteral("enabled"), true).toBool())
        return std::nullopt;
    const QString pattern = rule.value(QStringLiteral("pattern")).toString();
    if (pattern.isEmpty())
        return std::nullopt;
    const QRegularExpression re(pattern);
    if (!re.isValid())
        return std::nullopt;
    const QRegularExpressionMatch m = re.match(fileName);
    if (!m.hasMatch())
        return std::nullopt;
    const int sortGroupIndex = rule.value(QStringLiteral("sortGroupIndex"), -1).toInt();
    QStringList parts;
    for (int i = 1; i <= m.lastCapturedIndex(); ++i) {
        const QString captured = m.captured(i);
        if (i == sortGroupIndex) {
            if (sortValueOut)
                *sortValueOut = captured;
            continue; // the sort differentiator is explicitly NOT a grouping one
        }
        if (captured.isEmpty())
            continue; // an optional group that didn't participate adds nothing
        if (!parts.isEmpty() && captured == parts.last())
            continue; // collapse a consecutive duplicate capture
        parts.append(captured);
        if (capturedValuesOut)
            capturedValuesOut->append(captured);
    }
    return parts.join(QChar(' '));
}

// Deterministic merged-output filename for a (ruleName, capturedKey) pair,
// resolved against a directory + extension. Derived from the rule+key rather
// than any single member's filename so it stays recomputable from ONE surviving
// part after the others have moved into .combined/ — which is also what lets
// the USB grabber recognise an already-combined group (see runUsbGrabScan()).
// Readable form: "<rule> - <key>.combined.<ext>", e.g.
// "Acme Mic 3 - 20260710 edit.combined.wav".
inline QString combineOutputPathFor(const QString &ruleName, const QString &key, const QString &dir,
                                    const QString &ext)
{
    QString name = sanitizeForFilename(ruleName);
    if (!key.isEmpty())
        name += QStringLiteral(" - ") + sanitizeForFilename(key);
    return dir + QStringLiteral("/") + name + QStringLiteral(".combined.") + ext;
}
