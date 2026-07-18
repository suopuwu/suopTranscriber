#pragma once

#include <QFile>
#include <QString>
#include <QStringList>
#include <QTextStream>

// The transcript .md's "---\n<key: value lines>\n---\n<body>" frontmatter has
// no owning parser — the read-the-raw-block-then-mutate prologue was hand-rolled
// ~11 times across TranscriptionController plus a private copy in DiarizeTask.
// These two pure helpers single-source the raw-block read and the key upsert
// (the writers keep the raw lines rather than a parsed map so an unrelated key
// they don't touch is rewritten byte-for-byte). Header-only so no per-test
// dependency wiring. Writes still go through writeFrontmatterBlock() (atomic).
//
// NOTE: the value READERS (TranscriptionController::readMetadata,
// VaultTreeModel::frontmatterField) still parse independently and disagree on
// bool coercion — deliberately not unified here to keep this change to the
// write path.
namespace MdFrontmatter {

// Reads `path`'s frontmatter block into `lines` (the raw "key: value" lines
// between the two "---" delimiters) and everything after the closing delimiter
// into `body`, byte-for-byte. Returns false with *errorOut set if the file
// can't be opened or has no well-formed frontmatter block. Either out-param may
// be null.
inline bool read(const QString &path, QStringList *lines, QString *body, QString *errorOut)
{
    QFile mdFile(path);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for reading").arg(path);
        return false;
    }
    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---")) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has no frontmatter block").arg(path);
        return false;
    }
    QStringList fm;
    QString line;
    bool closed = false;
    while (!(line = in.readLine()).isNull()) {
        if (line == QStringLiteral("---")) {
            closed = true;
            break;
        }
        fm.append(line);
    }
    if (!closed) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has a malformed frontmatter block").arg(path);
        return false;
    }
    if (lines)
        *lines = fm;
    if (body)
        *body = in.readAll();
    return true;
}

// Replaces the "key: value" line in `lines` (matched on the key left of the
// first ':', trimmed) or appends it if absent. If removeIfEmpty is true and
// value is empty, removes the key instead of writing an empty value (and
// appends nothing).
inline void upsertKey(QStringList *lines, const QString &key, const QString &value, bool removeIfEmpty = false)
{
    const bool remove = removeIfEmpty && value.isEmpty();
    for (int i = 0; i < lines->size(); ++i) {
        const int colon = lines->at(i).indexOf(QLatin1Char(':'));
        if (colon >= 0 && lines->at(i).left(colon).trimmed() == key) {
            if (remove)
                lines->removeAt(i);
            else
                (*lines)[i] = key + QStringLiteral(": ") + value;
            return;
        }
    }
    if (!remove)
        lines->append(key + QStringLiteral(": ") + value);
}

} // namespace MdFrontmatter
