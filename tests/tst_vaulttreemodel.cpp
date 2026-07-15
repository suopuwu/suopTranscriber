// Living spec: VaultTreeModel — the sidebar tree over an Obsidian-style vault.
//
// Sources of truth:
//   CLAUDE.md  "Vault layout: sibling .md = transcribed (green/yellow indicator)."
//   DESIGN.md  "Vault / file layout" — directories with no audio anywhere in
//              their subtree are hidden; QFileSystemWatcher keeps the tree live.
//
// Each test slot documents one observable behavior. Tests drive the model only
// through its public QAbstractItemModel surface (role ints looked up by name
// via roleNames(); no hardcoded values beyond the documented Status enum).

#include <QtTest>

#include <QAbstractItemModelTester>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include "VaultTreeModel.h"

namespace {

// Creates a small file (and any missing parent dirs) inside the temp vault.
bool touch(const QString &path, const QByteArray &content = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content);
    return true;
}

// Role ints are looked up by their QML-facing names ("name", "status", ...)
// so the tests exercise the same contract QML delegates rely on.
int roleByName(const QAbstractItemModel &m, const QByteArray &roleName)
{
    const QHash<int, QByteArray> roles = m.roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        if (it.value() == roleName)
            return it.key();
    }
    return -1;
}

// Generic depth-first search by display name — indexes are invalidated on
// every rebuild (model reset), so callers must re-find rows after fs changes.
QModelIndex findByName(const QAbstractItemModel &m, const QString &name, const QModelIndex &parent = QModelIndex())
{
    const int nameRole = roleByName(m, "name");
    for (int row = 0; row < m.rowCount(parent); ++row) {
        const QModelIndex idx = m.index(row, 0, parent);
        if (m.data(idx, nameRole).toString() == name)
            return idx;
        const QModelIndex hit = findByName(m, name, idx);
        if (hit.isValid())
            return hit;
    }
    return {};
}

// Re-queries from scratch each call, so it is safe inside QTRY_ macros that
// poll across watcher-triggered model resets. -1 = node not found.
int statusOf(const QAbstractItemModel &m, const QString &name)
{
    const QModelIndex idx = findByName(m, name);
    if (!idx.isValid())
        return -1;
    return m.data(idx, roleByName(m, "status")).toInt();
}

} // namespace

class TstVaultTreeModel : public QObject
{
    Q_OBJECT

private slots:
    // --- Sibling .md convention (CLAUDE.md: "sibling .md = transcribed") ----

    // CLAUDE.md: an audio file with a sibling <fileName>.md (the FULL
    // filename, extension included) has been transcribed — shown with the
    // green indicator (TranscribedStatus = 2).
    void audioWithSiblingMd_isTranscribed_green()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("meeting.mp3")));
        QVERIFY(touch(vault.filePath("meeting.mp3.md")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex idx = findByName(model, "meeting.mp3");
        QVERIFY(idx.isValid());
        QCOMPARE(model.data(idx, roleByName(model, "status")).toInt(),
                 int(VaultTreeModel::TranscribedStatus));
        QCOMPARE(model.data(idx, roleByName(model, "isAudio")).toBool(), true);
        QCOMPARE(model.data(idx, roleByName(model, "isDir")).toBool(), false);
    }

    // CLAUDE.md: no sibling .md yet = not transcribed — yellow indicator
    // (UntranscribedStatus = 1).
    void audioWithoutSiblingMd_isUntranscribed_yellow()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("meeting.mp3")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex idx = findByName(model, "meeting.mp3");
        QVERIFY(idx.isValid());
        QCOMPARE(model.data(idx, roleByName(model, "status")).toInt(),
                 int(VaultTreeModel::UntranscribedStatus));
    }

    // The sibling match is on the FULL filename (fileName(), extension
    // included): "interview.part1.mp3" pairs with "interview.part1.mp3.md",
    // not "interview.part1.md" or "interview.md".
    void siblingMatch_usesFullFileName()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("interview.part1.mp3")));
        QVERIFY(touch(vault.filePath("interview.md"))); // NOT a sibling of the audio

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex audio = findByName(model, "interview.part1.mp3");
        QVERIFY(audio.isValid());
        QCOMPARE(model.data(audio, roleByName(model, "status")).toInt(),
                 int(VaultTreeModel::UntranscribedStatus));
        // interview.md matches no audio's full filename, so it is an orphan node.
        const QModelIndex md = findByName(model, "interview.md");
        QVERIFY(md.isValid());
        QCOMPARE(model.data(md, roleByName(model, "isOrphanedMd")).toBool(), true);
    }

    // Item P7 regression: two audio files sharing the same stem but
    // different extensions must NOT collide on one sidecar .md — each gets
    // its own sidecar named from its FULL filename (stem+ext+".md"), so
    // "meeting.wav" and "meeting.mp3" resolve to distinct "meeting.wav.md" /
    // "meeting.mp3.md" instead of both reading/writing the same "meeting.md".
    void sameStemDifferentExtension_getDistinctSidecars_noCollision()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("meeting.wav")));
        QVERIFY(touch(vault.filePath("meeting.wav.md"))); // only the .wav file is transcribed
        QVERIFY(touch(vault.filePath("meeting.mp3"))); // same stem, untranscribed

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QCOMPARE(statusOf(model, "meeting.wav"), int(VaultTreeModel::TranscribedStatus));
        QCOMPARE(statusOf(model, "meeting.mp3"), int(VaultTreeModel::UntranscribedStatus));
        // meeting.wav.md is the wav's sidecar, not a standalone orphaned node.
        QVERIFY(!findByName(model, "meeting.wav.md").isValid());
    }

    // --- Audio file recognition ---------------------------------------------

    // All documented audio extensions are recognized as audio leaves.
    void allDocumentedAudioExtensions_areRecognized()
    {
        const QStringList exts = {"mp3", "wav", "m4a", "flac", "ogg",
                                  "opus", "aac", "wma", "aiff", "alac"};

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        for (const QString &ext : exts)
            QVERIFY(touch(vault.filePath("clip_" + ext + '.' + ext)));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QCOMPARE(model.rowCount(), int(exts.size()));
        for (const QString &ext : exts) {
            const QModelIndex idx = findByName(model, "clip_" + ext + '.' + ext);
            QVERIFY2(idx.isValid(), qPrintable("missing node for ." + ext));
            QCOMPARE(model.data(idx, roleByName(model, "isAudio")).toBool(), true);
        }
    }

    // Extension matching is case-insensitive: SONG.MP3 is audio too.
    void audioExtensionMatch_isCaseInsensitive()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("SHOUTING.MP3")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex idx = findByName(model, "SHOUTING.MP3");
        QVERIFY(idx.isValid());
        QCOMPARE(model.data(idx, roleByName(model, "isAudio")).toBool(), true);
    }

    // Files that are neither audio nor .md (e.g. .txt, .jpg) never appear in
    // the tree — the sidebar shows only vault content that matters.
    void nonAudioNonMdFiles_doNotAppear()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("song.mp3")));
        QVERIFY(touch(vault.filePath("cover.jpg")));
        QVERIFY(touch(vault.filePath("notes.txt")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QCOMPARE(model.rowCount(), 1);
        QVERIFY(findByName(model, "song.mp3").isValid());
        QVERIFY(!findByName(model, "cover.jpg").isValid());
        QVERIFY(!findByName(model, "notes.txt").isValid());
    }

    // --- Orphaned .md handling ------------------------------------------------

    // A .md with no audio sibling of the same base name is an "orphaned" note:
    // it gets its own node with isOrphanedMd=true and OrphanedMdStatus (3).
    void orphanedMd_getsOwnNodeWithOrphanedStatus()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("stray-note.md")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex idx = findByName(model, "stray-note.md");
        QVERIFY(idx.isValid());
        QCOMPARE(model.data(idx, roleByName(model, "isOrphanedMd")).toBool(), true);
        QCOMPARE(model.data(idx, roleByName(model, "isAudio")).toBool(), false);
        QCOMPARE(model.data(idx, roleByName(model, "status")).toInt(),
                 int(VaultTreeModel::OrphanedMdStatus));
    }

    // A .md that IS the sibling of an audio file is transcript data for that
    // audio node — it must not appear as a separate tree node.
    void siblingMdOfAudio_isNotItsOwnNode()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("lecture.wav")));
        QVERIFY(touch(vault.filePath("lecture.wav.md")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QCOMPARE(model.rowCount(), 1); // only the audio node
        QVERIFY(findByName(model, "lecture.wav").isValid());
        QVERIFY(!findByName(model, "lecture.wav.md").isValid());
    }

    // --- Directory pruning (DESIGN.md: dirs with no relevant content are hidden)

    // A directory whose subtree contains no audio and no orphaned .md is
    // pruned from the tree entirely, even if it contains other files.
    void directoryWithNoAudioAndNoOrphanedMd_isHidden()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("keep/song.mp3")));
        QVERIFY(touch(vault.filePath("prune/readme.txt")));
        QVERIFY(QDir().mkpath(vault.filePath("empty")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QCOMPARE(model.rowCount(), 1);
        QVERIFY(findByName(model, "keep").isValid());
        QVERIFY(!findByName(model, "prune").isValid());
        QVERIFY(!findByName(model, "empty").isValid());
    }

    // A directory whose only content is an orphaned .md is kept — pruning is
    // about "relevant content", not literally "has audio".
    void directoryWithOnlyOrphanedMd_isKept()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("notes-only/idea.md")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex dir = findByName(model, "notes-only");
        QVERIFY(dir.isValid());
        QCOMPARE(model.data(dir, roleByName(model, "isDir")).toBool(), true);
        const QModelIndex md = findByName(model, "idea.md");
        QVERIFY(md.isValid());
        QCOMPARE(md.parent(), dir);
    }

    // Keeping a directory is transitive: a/b/c with audio only at the deepest
    // level keeps the whole chain visible.
    void nestedDirectoriesWithAudioDeepDown_areKept()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a/b/c/deep.flac")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex a = findByName(model, "a");
        const QModelIndex b = findByName(model, "b");
        const QModelIndex c = findByName(model, "c");
        const QModelIndex deep = findByName(model, "deep.flac");
        QVERIFY(a.isValid());
        QVERIFY(b.isValid());
        QVERIFY(c.isValid());
        QVERIFY(deep.isValid());
        QCOMPARE(b.parent(), a);
        QCOMPARE(c.parent(), b);
        QCOMPARE(deep.parent(), c);
    }

    // --- Live updates (DESIGN.md: QFileSystemWatcher keeps the tree fresh) --

    // After the model is built, writing the sibling .md on disk (as the
    // transcription engine does when it finishes) flips the audio row from
    // yellow to green without the app touching vaultPath. The watcher is
    // async, so poll with a generous timeout.
    void creatingSiblingMdOnDisk_flipsStatusToTranscribed_live()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("session.opus")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QCOMPARE(statusOf(model, "session.opus"), int(VaultTreeModel::UntranscribedStatus));

        // Simulate a finished transcription: the sibling .md appears on disk.
        QVERIFY(touch(vault.filePath("session.opus.md"), "[00:00 - 00:01] hello\n"));

        QTRY_COMPARE_WITH_TIMEOUT(statusOf(model, "session.opus"),
                                  int(VaultTreeModel::TranscribedStatus), 10000);
    }

    // --- renameFile ------------------------------------------------------------

    // renameFile keeps the vault convention intact: the audio file and its
    // sibling .md rename together, each preserving its own extension.
    void rename_movesAudioAndSiblingMdTogether_preservingExtensions()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("old name.m4a")));
        QVERIFY(touch(vault.filePath("old name.m4a.md"), "transcript"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QString error;
        QVERIFY(model.renameFile(QUrl::fromLocalFile(vault.filePath("old name.m4a")),
                                 "new name", &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));

        QVERIFY(QFileInfo::exists(vault.filePath("new name.m4a")));
        QVERIFY(QFileInfo::exists(vault.filePath("new name.m4a.md")));
        QVERIFY(!QFileInfo::exists(vault.filePath("old name.m4a")));
        QVERIFY(!QFileInfo::exists(vault.filePath("old name.m4a.md")));

        // The model rebuilds after a rename, so the tree reflects the new pair.
        QCOMPARE(statusOf(model, "new name.m4a"), int(VaultTreeModel::TranscribedStatus));
        QVERIFY(!findByName(model, "old name.m4a").isValid());
    }

    // Empty (and whitespace-only) names are rejected with an error message and
    // no filesystem change.
    void rename_rejectsEmptyName_leavingFilesUntouched()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("keepme.mp3")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        for (const QString &badName : {QString(), QStringLiteral("   ")}) {
            QString error;
            QVERIFY(!model.renameFile(QUrl::fromLocalFile(vault.filePath("keepme.mp3")),
                                      badName, &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(QFileInfo::exists(vault.filePath("keepme.mp3")));
        }
    }

    // Names containing any of / \ : * ? " < > | are rejected — these are the
    // characters that break paths on at least one supported filesystem.
    void rename_rejectsEveryInvalidCharacter_leavingFilesUntouched()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("keepme.mp3")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QString invalidChars = QStringLiteral("/\\:*?\"<>|");
        for (const QChar &c : invalidChars) {
            QString error;
            const QString badName = QStringLiteral("bad") + c + QStringLiteral("name");
            QVERIFY2(!model.renameFile(QUrl::fromLocalFile(vault.filePath("keepme.mp3")),
                                       badName, &error),
                     qPrintable(QStringLiteral("'%1' was not rejected").arg(c)));
            QVERIFY(!error.isEmpty());
            QVERIFY(QFileInfo::exists(vault.filePath("keepme.mp3")));
        }
    }

    // A rename that would overwrite an existing file (either the audio target
    // or the .md target) is refused before anything moves — the audio/.md pair
    // is never left mismatched.
    void rename_rejectsCollisionWithExistingFile()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a.mp3")));
        QVERIFY(touch(vault.filePath("a.mp3.md"), "a transcript"));
        QVERIFY(touch(vault.filePath("b.mp3")));
        QVERIFY(touch(vault.filePath("c.mp3.md"), "c transcript"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        // Audio target collision: a -> b, but b.mp3 exists.
        QString error;
        QVERIFY(!model.renameFile(QUrl::fromLocalFile(vault.filePath("a.mp3")), "b", &error));
        QVERIFY(!error.isEmpty());

        // Markdown target collision: a -> c; c.mp3 is free but c.mp3.md exists.
        error.clear();
        QVERIFY(!model.renameFile(QUrl::fromLocalFile(vault.filePath("a.mp3")), "c", &error));
        QVERIFY(!error.isEmpty());

        // Nothing changed on disk in either failed attempt.
        QVERIFY(QFileInfo::exists(vault.filePath("a.mp3")));
        QVERIFY(QFileInfo::exists(vault.filePath("a.mp3.md")));
        QVERIFY(!QFileInfo::exists(vault.filePath("c.mp3")));
    }

    // Orphaned .md-only nodes are renameable on their own ("forgo labels"
    // mode, Milestone 5 item 19).
    void rename_handlesStandaloneOrphanedMd()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("draft.md"), "just a note"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QString error;
        QVERIFY(model.renameFile(QUrl::fromLocalFile(vault.filePath("draft.md")),
                                 "final", &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(vault.filePath("final.md")));
        QVERIFY(!QFileInfo::exists(vault.filePath("draft.md")));

        const QModelIndex idx = findByName(model, "final.md");
        QVERIFY(idx.isValid());
        QCOMPARE(model.data(idx, roleByName(model, "isOrphanedMd")).toBool(), true);
    }

    // --- importFile (drag-and-drop import into the vault root) -----------------

    // Dropping a recognized audio file copies it into the vault root and the
    // tree picks it up (via the explicit rebuild(), not just the watcher) —
    // and the ORIGINAL file is left in place, since this is a copy, not a
    // move (DESIGN.md's "USB import" precedent: "copy only new audio in").
    void importFile_copiesRecognizedAudioIntoVaultRoot_leavingSourceIntact()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QVERIFY(touch(source.filePath("dropped.mp3"), "audio bytes"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QString error;
        QVERIFY2(model.importFile(QUrl::fromLocalFile(source.filePath("dropped.mp3")), &error),
                 qPrintable(error));
        QVERIFY(error.isEmpty());

        QVERIFY(QFileInfo::exists(vault.filePath("dropped.mp3")));
        QVERIFY(QFileInfo::exists(source.filePath("dropped.mp3"))); // source untouched
        const QModelIndex idx = findByName(model, "dropped.mp3");
        QVERIFY(idx.isValid());
        QCOMPARE(model.data(idx, roleByName(model, "status")).toInt(),
                 int(VaultTreeModel::UntranscribedStatus));
    }

    // A file with an unrecognized extension is refused, not silently copied
    // in as vault clutter.
    void importFile_rejectsNonAudioExtension()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QVERIFY(touch(source.filePath("notes.txt")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QString error;
        QVERIFY(!model.importFile(QUrl::fromLocalFile(source.filePath("notes.txt")), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(vault.filePath("notes.txt")));
    }

    // A same-named file already at the vault root is never silently
    // overwritten or auto-renamed — the import is refused outright.
    void importFile_rejectsCollisionWithExistingFile()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("song.mp3"), "original"));
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QVERIFY(touch(source.filePath("song.mp3"), "different bytes"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QString error;
        QVERIFY(!model.importFile(QUrl::fromLocalFile(source.filePath("song.mp3")), &error));
        QVERIFY(!error.isEmpty());

        QFile original(vault.filePath("song.mp3"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("original")); // untouched
    }

    // No vault open (empty vaultLocalPath): importFile() fails with an error
    // rather than crashing or writing to some unintended location.
    void importFile_noVaultOpen_isInertNotFatal()
    {
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QVERIFY(touch(source.filePath("clip.mp3")));

        VaultTreeModel model;
        QString error;
        QVERIFY(!model.importFile(QUrl::fromLocalFile(source.filePath("clip.mp3")), &error));
        QVERIFY(!error.isEmpty());
    }

    // --- Search support roles (Item 9: LabelRole/TagsRole/TranscriptBodyRole) --

    // An audio file's frontmatter label/tags are exposed via LabelRole/
    // TagsRole, read fresh from the sidecar .md rather than cached on Node.
    void audioWithFrontmatter_exposesLabelAndTagsRoles()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3")));
        QVERIFY(touch(vault.filePath("talk.mp3.md"),
                      "---\nmodel: base\nlabel: Quarterly Talk\ntags: finance, meetings\n"
                      "speakers: Alice, Bob\n---\n\n"
                      "[00:00:00.000 - 00:00:01.000] hello\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex idx = findByName(model, "talk.mp3");
        QVERIFY(idx.isValid());
        QCOMPARE(model.data(idx, roleByName(model, "label")).toString(), QStringLiteral("Quarterly Talk"));
        QCOMPARE(model.data(idx, roleByName(model, "tags")).toString(), QStringLiteral("finance, meetings"));
        // SpeakersRole mirrors TagsRole exactly (same frontmatter-only read,
        // "speakers:" is kept in sync with the body by
        // renameSpeakerInFile()/mergeSpeakersInFile() — see
        // TranscriptionController.cpp).
        QCOMPARE(model.data(idx, roleByName(model, "speakers")).toString(), QStringLiteral("Alice, Bob"));
    }

    // An untranscribed audio file (no sidecar .md yet) yields empty
    // label/tags/speakers rather than erroring or crashing.
    void audioWithoutSidecarMd_labelAndTagsRolesAreEmpty()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("untouched.mp3")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex idx = findByName(model, "untouched.mp3");
        QVERIFY(idx.isValid());
        QVERIFY(model.data(idx, roleByName(model, "label")).toString().isEmpty());
        QVERIFY(model.data(idx, roleByName(model, "tags")).toString().isEmpty());
        QVERIFY(model.data(idx, roleByName(model, "speakers")).toString().isEmpty());
    }

    // TranscriptBodyRole returns the segment text (frontmatter stripped) for a
    // transcribed audio file, matching TranscriptionController::readTranscriptBody().
    void transcribedAudio_transcriptBodyRoleReturnsBodyWithoutFrontmatter()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("session.wav")));
        QVERIFY(touch(vault.filePath("session.wav.md"),
                      "---\nmodel: base\n---\n\n[00:00:00.000 - 00:00:01.000] the quick fox\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex idx = findByName(model, "session.wav");
        QVERIFY(idx.isValid());
        const QString body = model.data(idx, roleByName(model, "transcriptBody")).toString();
        QVERIFY(body.contains(QStringLiteral("the quick fox")));
        QVERIFY(!body.contains(QStringLiteral("model:")));
    }

    // Item 9's search-transcriptions behavior only ever covered TranscribedStatus
    // rows (see Sidebar.qml's original matchesSearch: "status === 2"); an
    // orphaned .md's own content is not exposed via TranscriptBodyRole, since
    // its status is OrphanedMdStatus, not TranscribedStatus.
    void orphanedMd_transcriptBodyRoleIsEmpty()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("stray.md"), "---\n---\n\nsome stray note content\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex idx = findByName(model, "stray.md");
        QVERIFY(idx.isValid());
        QVERIFY(model.data(idx, roleByName(model, "transcriptBody")).toString().isEmpty());
    }

    // --- Vault-scoped settings (Item 9: .transcriber/settings.json JSON file) ---

    // writeVaultSettings() then readVaultSettings() round-trips arbitrary
    // key/value data through a JSON file inside the vault's hidden
    // .transcriber/ config dir (created on demand).
    void vaultSettings_roundTripsThroughHiddenJsonFile()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVariantMap settings;
        settings[QStringLiteral("borderRadius")] = 12.5;
        settings[QStringLiteral("useSystemTitlebar")] = false;
        settings[QStringLiteral("sidebarBackgroundColor")] = QStringLiteral("#112233");

        QString error;
        QVERIFY2(model.writeVaultSettings(settings, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(vault.filePath(".transcriber/settings.json")));

        const QVariantMap readBack = model.readVaultSettings();
        QCOMPARE(readBack.value(QStringLiteral("borderRadius")).toDouble(), 12.5);
        QCOMPARE(readBack.value(QStringLiteral("useSystemTitlebar")).toBool(), false);
        QCOMPARE(readBack.value(QStringLiteral("sidebarBackgroundColor")).toString(), QStringLiteral("#112233"));
    }

    // The settings file lives inside .transcriber/, which is itself a hidden
    // dotdir (QDir's default filters exclude it), so neither it nor its
    // contents ever appear as a tree node even though it sits at the vault
    // root alongside real content.
    void vaultSettingsFile_neverAppearsAsATreeNode()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("song.mp3")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QVERIFY(model.writeVaultSettings({{QStringLiteral("borderRadius"), 8}}));

        QCOMPARE(model.rowCount(), 1);
        QVERIFY(findByName(model, "song.mp3").isValid());
        QVERIFY(!findByName(model, ".transcriber").isValid());
    }

    // No vault open (empty vaultLocalPath): readVaultSettings() returns an
    // empty map and writeVaultSettings() fails with an error, rather than
    // crashing or writing to some unintended location.
    void vaultSettings_noVaultOpen_isInertNotFatal()
    {
        VaultTreeModel model;
        QVERIFY(model.readVaultSettings().isEmpty());

        QString error;
        QVERIFY(!model.writeVaultSettings({{QStringLiteral("borderRadius"), 8}}, &error));
        QVERIFY(!error.isEmpty());
    }

    // Recording feature adds four new AppSettings keys (recordingOutputFolder,
    // animationsEnabled, recordingMicGain, recordingSystemGain) — same
    // round-trip mechanism as vaultSettings_roundTripsThroughHiddenJsonFile
    // above, just proving the underlying JSON read/write handles the new
    // keys' types (string/bool/real) correctly. AppSettings.qml itself has no
    // dedicated QML-engine test harness in this file (see that test's shape).
    void vaultSettings_roundTripsRecordingAndAnimationKeys()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVariantMap settings;
        settings[QStringLiteral("recordingOutputFolder")] = QStringLiteral("recordings");
        settings[QStringLiteral("animationsEnabled")] = false;
        settings[QStringLiteral("recordingMicGain")] = 1.5;
        settings[QStringLiteral("recordingSystemGain")] = 0.75;

        QString error;
        QVERIFY2(model.writeVaultSettings(settings, &error), qPrintable(error));

        const QVariantMap readBack = model.readVaultSettings();
        QCOMPARE(readBack.value(QStringLiteral("recordingOutputFolder")).toString(), QStringLiteral("recordings"));
        QCOMPARE(readBack.value(QStringLiteral("animationsEnabled")).toBool(), false);
        QCOMPARE(readBack.value(QStringLiteral("recordingMicGain")).toDouble(), 1.5);
        QCOMPARE(readBack.value(QStringLiteral("recordingSystemGain")).toDouble(), 0.75);
    }

    // --- USB grab history (.transcriber/usbGrabHistory.json, array-root JSON) ---

    // appendUsbGrabHistoryEntry() then readUsbGrabHistory() round-trips a
    // grab-history entry through an array-root JSON file inside the vault's
    // hidden .transcriber/ dir — same "created on demand" mechanism as
    // vaultSettings_roundTripsThroughHiddenJsonFile above, but an array root
    // (one entry per past grab) rather than an object root.
    void usbGrabHistory_roundTripsThroughHiddenJsonFile()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVariantMap entry;
        entry[QStringLiteral("timestamp")] = QStringLiteral("2026-07-14T12:00:00Z");
        entry[QStringLiteral("deviceSerial")] = QStringLiteral("ABC123");
        entry[QStringLiteral("deviceLabel")] = QStringLiteral("Zoom H1n");
        entry[QStringLiteral("filesCopied")] = 3;
        entry[QStringLiteral("filesSkipped")] = 1;
        entry[QStringLiteral("errors")] = QStringList();

        QString error;
        QVERIFY2(model.appendUsbGrabHistoryEntry(entry, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(vault.filePath(".transcriber/usbGrabHistory.json")));

        const QVariantList history = model.readUsbGrabHistory();
        QCOMPARE(history.size(), 1);
        const QVariantMap readBack = history.first().toMap();
        QCOMPARE(readBack.value(QStringLiteral("deviceSerial")).toString(), QStringLiteral("ABC123"));
        QCOMPARE(readBack.value(QStringLiteral("deviceLabel")).toString(), QStringLiteral("Zoom H1n"));
        QCOMPARE(readBack.value(QStringLiteral("filesCopied")).toInt(), 3);
        QCOMPARE(readBack.value(QStringLiteral("filesSkipped")).toInt(), 1);
    }

    // Multiple grabs accumulate in append order (oldest first) rather than
    // each append overwriting the last — UsbGrabberController reverses this
    // client-side for newest-first display.
    void usbGrabHistory_multipleAppendsAccumulateInOrder()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        for (const QString &serial : {QStringLiteral("FIRST"), QStringLiteral("SECOND"), QStringLiteral("THIRD")}) {
            QVariantMap entry;
            entry[QStringLiteral("deviceSerial")] = serial;
            entry[QStringLiteral("filesCopied")] = 0;
            entry[QStringLiteral("filesSkipped")] = 0;
            entry[QStringLiteral("errors")] = QStringList();
            QVERIFY(model.appendUsbGrabHistoryEntry(entry));
        }

        const QVariantList history = model.readUsbGrabHistory();
        QCOMPARE(history.size(), 3);
        QCOMPARE(history.at(0).toMap().value(QStringLiteral("deviceSerial")).toString(), QStringLiteral("FIRST"));
        QCOMPARE(history.at(1).toMap().value(QStringLiteral("deviceSerial")).toString(), QStringLiteral("SECOND"));
        QCOMPARE(history.at(2).toMap().value(QStringLiteral("deviceSerial")).toString(), QStringLiteral("THIRD"));
    }

    // No vault open, or a vault that's never had a grab performed against it:
    // an empty list, not an error — the file simply doesn't exist yet.
    void usbGrabHistory_noHistoryYet_returnsEmptyListNotError()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QVERIFY(model.readUsbGrabHistory().isEmpty());

        VaultTreeModel noVaultModel;
        QVERIFY(noVaultModel.readUsbGrabHistory().isEmpty());

        QString error;
        QVERIFY(!noVaultModel.appendUsbGrabHistoryEntry({{QStringLiteral("deviceSerial"), QStringLiteral("X")}}, &error));
        QVERIFY(!error.isEmpty());
    }

    // The history file lives inside .transcriber/, same hidden-dotdir
    // exclusion as settings.json — never appears as a tree node.
    void usbGrabHistoryFile_neverAppearsAsATreeNode()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("song.mp3")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QVERIFY(model.appendUsbGrabHistoryEntry({{QStringLiteral("deviceSerial"), QStringLiteral("X")}}));

        QCOMPARE(model.rowCount(), 1);
        QVERIFY(findByName(model, "song.mp3").isValid());
        QVERIFY(!findByName(model, ".transcriber").isValid());
    }

    // --- isPathInsideVault() (recording feature: output-folder validation) -------

    // The vault root itself, and any subdirectory (nested or not), count as
    // "inside" the vault.
    void isPathInsideVault_acceptsVaultRootAndSubdirectory()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath("recordings/nested")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(model.isPathInsideVault(vault.path()));
        QVERIFY(model.isPathInsideVault(vault.filePath("recordings")));
        QVERIFY(model.isPathInsideVault(vault.filePath("recordings/nested")));
        // Doesn't need to exist yet — a folder the user is about to create.
        QVERIFY(model.isPathInsideVault(vault.filePath("recordings/not-yet-created")));
    }

    // A sibling directory outside the vault root, or a path that uses ".."
    // to climb back out of the vault, are both rejected.
    void isPathInsideVault_rejectsSiblingAndDotDotEscape()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath("recordings")));

        QTemporaryDir sibling;
        QVERIFY(sibling.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(!model.isPathInsideVault(sibling.path()));
        QVERIFY(!model.isPathInsideVault(vault.filePath("recordings/../..")));
        QVERIFY(!model.isPathInsideVault(vault.filePath("recordings/../../escaped")));
    }

    // --- allAudioPaths() (TitleBar "force retranscribe all") ---------------------

    // Every audio file's absolute path is returned regardless of transcribed
    // status, nested directory depth, or orphaned .md/non-audio noise — this
    // is what "force retranscribe all" enumerates over, unlike the sidebar's
    // viewport-realized-delegate tracking, so it must reflect the full vault
    // even for rows that were never scrolled into view.
    void allAudioPaths_listsEveryAudioFile_transcribedOrNot_atAnyDepth()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("untranscribed.mp3")));
        QVERIFY(touch(vault.filePath("transcribed.mp3")));
        QVERIFY(touch(vault.filePath("transcribed.mp3.md")));
        QVERIFY(touch(vault.filePath("orphaned.md")));
        QVERIFY(touch(vault.filePath("notes.txt")));
        QVERIFY(touch(vault.filePath("nested/deep/buried.wav")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QStringList paths = model.allAudioPaths();
        paths.sort();

        QStringList expected;
        expected << vault.filePath("nested/deep/buried.wav") << vault.filePath("transcribed.mp3")
                 << vault.filePath("untranscribed.mp3");
        expected.sort();

        QCOMPARE(paths, expected);
    }

    void allAudioPaths_emptyVault_returnsEmptyList()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(model.allAudioPaths().isEmpty());
    }

    // --- audioLastModified() (Sidebar.qml file-tree sort: Date Modified) --------

    // One entry per audio path (same set allAudioPaths() returns), each
    // matching QFileInfo::lastModified() for that file — Sidebar.qml's
    // sort-mode feature has no other way to read raw mtimes from QML.
    void audioLastModified_matchesQFileInfoForEveryAudioFile()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a.mp3")));
        QVERIFY(touch(vault.filePath("nested/b.wav")));
        QVERIFY(touch(vault.filePath("orphaned.md")));
        QVERIFY(touch(vault.filePath("notes.txt")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap mtimes = model.audioLastModified();
        QCOMPARE(mtimes.size(), 2);
        for (const QString &path : { vault.filePath("a.mp3"), vault.filePath("nested/b.wav") }) {
            QVERIFY(mtimes.contains(path));
            QCOMPARE(mtimes.value(path).toLongLong(), QFileInfo(path).lastModified().toMSecsSinceEpoch());
        }
    }

    void audioLastModified_emptyVault_returnsEmptyMap()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(model.audioLastModified().isEmpty());
    }

    // --- Model contract ----------------------------------------------------------

    // QAbstractItemModelTester validates the full QAbstractItemModel contract
    // (index/parent symmetry, reset signal ordering, ...) over a populated
    // vault, including across a watcher-triggered rebuild.
    void modelPassesQAbstractItemModelTester_includingRebuild()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("root.mp3")));
        QVERIFY(touch(vault.filePath("root.mp3.md")));
        QVERIFY(touch(vault.filePath("orphan.md")));
        QVERIFY(touch(vault.filePath("sub/nested.wav")));
        QVERIFY(touch(vault.filePath("sub/deeper/leaf.ogg")));

        VaultTreeModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);

        // Initial build (setVaultPath -> synchronous rebuild) under the tester.
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QVERIFY(findByName(model, "leaf.ogg").isValid());

        // Filesystem change triggers an async rebuild; the tester validates
        // the model reset it performs.
        QVERIFY(touch(vault.filePath("late-arrival.mp3")));
        QTRY_VERIFY_WITH_TIMEOUT(findByName(model, "late-arrival.mp3").isValid(), 10000);
        QCOMPARE(statusOf(model, "late-arrival.mp3"), int(VaultTreeModel::UntranscribedStatus));
    }
};

QTEST_GUILESS_MAIN(TstVaultTreeModel)
#include "tst_vaulttreemodel.moc"
