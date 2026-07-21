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
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
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

// True if any dataChanged captured by `spy` carried `role` in its roles list
// (an empty roles list means "all roles" in Qt's convention, so it counts too).
bool dataChangedCarriedRole(const QSignalSpy &spy, int role)
{
    for (const QList<QVariant> &args : spy) {
        const QList<int> roles = args.at(2).value<QList<int>>();
        if (roles.isEmpty() || roles.contains(role))
            return true;
    }
    return false;
}

// A restore that puts the file back but mangles its bytes would still pass an
// exists() check — the round-trip tests compare content, so they need this.
QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// Deletion moved from "trash, or permanently remove if that fails" behind one
// bool-returning call to an explicit trash-only attempt returning a result map
// (see VaultTreeModel.h's deletion block). What the filesystem ends up looking
// like is unchanged, so the assertions below are too — only the call shape is.
// These wrappers keep that difference in one place instead of smearing
// `.value("trashed").toInt() == 1` across thirty call sites.
// Files and folders go through deliberately separate calls (a folder delete is
// recursive — see VaultTreeModel.h's trashFolder()), so this dispatches on what
// the path actually is, keeping the call sites below reading as "delete this".
bool trashOne(VaultTreeModel &m, const QUrl &url)
{
    const QVariantMap r = QFileInfo(url.toLocalFile()).isDir() ? m.trashFolder(url)
                                                               : m.trashFiles({ url });
    return r.value(QStringLiteral("trashed")).toInt() == 1;
}

int trashMany(VaultTreeModel &m, const QList<QUrl> &urls)
{
    return m.trashFiles(urls).value(QStringLiteral("trashed")).toInt();
}

// The trash*/delete* calls are all batch-shaped (one rebuild per user action —
// see trashFiles()' doc comment); these adapt them for the many single-target
// cases below, where the batch is just one url.
bool trashTranscriptOne(VaultTreeModel &m, const QUrl &url)
{
    return m.trashTranscript({ url }).value(QStringLiteral("trashed")).toInt() == 1;
}

bool trashAudioOnlyOne(VaultTreeModel &m, const QUrl &url)
{
    return m.trashAudioOnly({ url }).value(QStringLiteral("trashed")).toInt() == 1;
}

bool deleteAudioOnlyPermanentlyOne(VaultTreeModel &m, const QUrl &url)
{
    return m.deleteAudioOnlyPermanently({ url }) == 1;
}

// The "restorable" list from a trash result, ready to hand to restoreFromTrash().
QVariantList restorableOf(const QVariantMap &result)
{
    return result.value(QStringLiteral("restorable")).toList();
}

// Push a file's mtime forward so a same-size in-place rewrite is still
// detectable by the model's mtime capture — a rewrite microseconds after the
// initial scan could otherwise land on a coarse-resolution-identical timestamp.
bool bumpMtime(const QString &path, int secondsForward = 3)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite))
        return false;
    return f.setFileTime(QDateTime::currentDateTime().addSecs(secondsForward),
                         QFileDevice::FileModificationTime);
}

} // namespace

class TstVaultTreeModel : public QObject
{
    Q_OBJECT

private:
    // Backs the XDG_DATA_HOME redirect below; must outlive every test slot.
    QTemporaryDir m_xdgDataHome;

    // Path an entry named `name` lands at once trashed, under the redirected
    // home trash. (The XDG spec uniquifies a collision to "name-1" etc., so
    // this only holds for a name trashed once per run — which is how the tests
    // below use it.)
    QString trashedPath(const QString &name) const
    {
        return m_xdgDataHome.path() + QStringLiteral("/Trash/files/") + name;
    }

private slots:
    // The deletes below are trash-first, which makes the trash DESTINATION part
    // of the behavior under test — but the real home trash is both the
    // developer's own (CLAUDE.md: tests never touch real user state) and, per
    // the XDG spec, unusable for a /tmp QTemporaryDir vault on a different
    // filesystem — moveToTrash() would silently fall back to a permanent
    // delete and the trash path would stop being tested at all. Pointing
    // XDG_DATA_HOME at a QTemporaryDir fixes both at once: the home trash
    // becomes <temp>/Trash, sitting on the same filesystem as the test vaults,
    // so trashing genuinely happens and is assertable — and it's thrown away
    // with the temp dir.
    void initTestCase()
    {
        QVERIFY(m_xdgDataHome.isValid());
        qputenv("XDG_DATA_HOME", m_xdgDataHome.path().toUtf8());
    }

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

    // A sibling .md whose frontmatter carries `partial: true` (a cancelled or
    // killed transcribe left a truncated file) reports the distinct
    // PartialStatus, not TranscribedStatus — while a complete .md stays green.
    void partialFrontmatter_reportsPartialStatus_notTranscribed()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("done.mp3")));
        QVERIFY(touch(vault.filePath("done.mp3.md"), "---\nmodel: x\n---\n\n[0:00] hi\n"));
        QVERIFY(touch(vault.filePath("partial.mp3")));
        QVERIFY(touch(vault.filePath("partial.mp3.md"),
                      "---\nmodel: x\npartial: true\n---\n\n[0:00] hi\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QCOMPARE(statusOf(model, "done.mp3"), int(VaultTreeModel::TranscribedStatus));
        QCOMPARE(statusOf(model, "partial.mp3"), int(VaultTreeModel::PartialStatus));
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

    // --- Directory visibility (the tree mirrors the vault's real folders)

    // Every directory is shown, whatever it does or doesn't contain: an empty
    // one, one holding only unrelated files, and one nesting only empty dirs.
    // Filtering applies to leaf files, not folders — so "prune/readme.txt"
    // itself is still not a node even though its folder is.
    void directoryWithNoAudioAndNoOrphanedMd_isShown()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("keep/song.mp3")));
        QVERIFY(touch(vault.filePath("prune/readme.txt")));
        QVERIFY(QDir().mkpath(vault.filePath("empty")));
        QVERIFY(QDir().mkpath(vault.filePath("outer/inner")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QCOMPARE(model.rowCount(), 4);
        QVERIFY(findByName(model, "keep").isValid());
        QVERIFY(findByName(model, "empty").isValid());

        const QModelIndex prune = findByName(model, "prune");
        QVERIFY(prune.isValid());
        QCOMPARE(model.data(prune, roleByName(model, "isDir")).toBool(), true);
        QCOMPARE(model.rowCount(prune), 0); // the .txt is still filtered out

        // Pruning used to propagate transitively; nested empty dirs now survive.
        const QModelIndex outer = findByName(model, "outer");
        QVERIFY(outer.isValid());
        QCOMPARE(model.rowCount(outer), 1);
        QCOMPARE(model.data(model.index(0, 0, outer), roleByName(model, "name")).toString(), QString("inner"));
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

    // Queue item 038: a transcript APPEARING flips status only — a row's
    // indicator colour, never the tree's shape — so it must refresh in place
    // with dataChanged(StatusRole) and NOT reset the model (a reset drops the
    // TreeView's expansion state and scroll offset).
    void transcriptCreated_emitsDataChangedNotReset()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("session.opus")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QCOMPARE(statusOf(model, "session.opus"), int(VaultTreeModel::UntranscribedStatus));

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);

        QVERIFY(touch(vault.filePath("session.opus.md"), "[0] hello\n"));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(model, "session.opus"),
                                  int(VaultTreeModel::TranscribedStatus), 10000);

        QCOMPARE(resetSpy.count(), 0);
        QVERIFY2(dataChangedCarriedRole(dataSpy, int(VaultTreeModel::StatusRole)),
                 "the status flip must arrive as a dataChanged carrying StatusRole");
    }

    // Queue item 038: the mirror — a transcript DISAPPEARING (Delete transcript)
    // is also status-only and must not reset the model.
    void transcriptDeleted_emitsDataChangedNotReset()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a.wav")));
        QVERIFY(touch(vault.filePath("a.wav.md"), "[0] one\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QCOMPARE(statusOf(model, "a.wav"), int(VaultTreeModel::TranscribedStatus));

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);

        QVERIFY(trashTranscriptOne(model, QUrl::fromLocalFile(vault.filePath("a.wav"))));
        QCOMPARE(statusOf(model, "a.wav"), int(VaultTreeModel::UntranscribedStatus));

        QCOMPARE(resetSpy.count(), 0);
        QVERIFY2(dataChangedCarriedRole(dataSpy, int(VaultTreeModel::StatusRole)),
                 "the status flip must arrive as a dataChanged carrying StatusRole");
    }

    // Queue item 038: the whole point is a STABLE QModelIndex across the flip —
    // a QPersistentModelIndex taken before the transcript appears must survive
    // it. A model reset would invalidate it; this fails loudly if someone
    // re-adds status to sameStructure().
    void expansionSurvivesTranscriptStatusFlip()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("folder/nested.wav")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QPersistentModelIndex nested(findByName(model, "nested.wav"));
        QVERIFY(nested.isValid());
        QCOMPARE(statusOf(model, "nested.wav"), int(VaultTreeModel::UntranscribedStatus));

        QVERIFY(touch(vault.filePath("folder/nested.wav.md"), "[0] hi\n"));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(model, "nested.wav"),
                                  int(VaultTreeModel::TranscribedStatus), 10000);

        QVERIFY2(nested.isValid(),
                 "the persistent index was invalidated — a status flip must not reset the model");
    }

    // --- watcher-driven rebuild coalescing (perf fix: move/delete jank) --------

    // A multi-file move dirties BOTH the source and destination dir per file, so
    // an N-file move fires ~2N directoryChanged events in a burst. Each one used
    // to trigger its own full model reset (and, in QML, a full-vault walk + proxy
    // re-sort + whole-tree re-expand). The watcher->rebuild path now debounces,
    // so the whole burst collapses into a SINGLE rebuild. This mimics MoveTask,
    // which does a pure filesystem move and leans on the watcher — so it renames
    // on disk directly rather than calling model.moveFile() (which rebuilds
    // synchronously and would defeat the point of the test).
    void multiFileMove_coalescesIntoOneRebuild()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        // A destination folder must already hold audio so it is watched from the
        // initial build (an unwatched dir fires no directoryChanged on the move).
        QVERIFY(touch(vault.filePath("sub/seed.wav"), "audio"));
        const int n = 8;
        for (int i = 0; i < n; ++i)
            QVERIFY(touch(vault.filePath(QStringLiteral("clip%1.wav").arg(i)), "audio"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QCOMPARE(statusOf(model, "clip0.wav"), int(VaultTreeModel::UntranscribedStatus));

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        for (int i = 0; i < n; ++i) {
            QVERIFY(QFile::rename(vault.filePath(QStringLiteral("clip%1.wav").arg(i)),
                                  vault.filePath(QStringLiteral("sub/clip%1.wav").arg(i))));
        }

        // The debounce (50ms) fires once after the burst settles; wait well past
        // it AND any straggling event, then assert exactly one rebuild — not ~2N.
        QTRY_VERIFY_WITH_TIMEOUT(resetSpy.count() >= 1, 5000);
        QTest::qWait(300);
        QCOMPARE(resetSpy.count(), 1);
        // The moves really happened (guards against the test passing on a no-op).
        QCOMPARE(statusOf(model, "clip0.wav"), int(VaultTreeModel::UntranscribedStatus));
        QVERIFY(findByName(model, "sub").isValid());
    }

    // --- no-op rebuild suppression (perf fix: delete/undo reflows twice) --------

    // A STRUCTURAL delete resets the model exactly ONCE — the explicit
    // synchronous rebuild in applyRemovalOutcome(). The watcher's
    // directoryChanged echo of the same change rescans, finds the tree
    // structurally identical (the explicit rebuild already applied the removal)
    // and returns without a second reset. A transcript-only trash is NOT
    // structural (item 038): it only flips status, refreshed in place with
    // dataChanged and no reset at all.
    void deleteTrash_resetsOnlyForStructuralChanges()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a.wav")));
        QVERIFY(touch(vault.filePath("a.wav.md"), "[0] one\n"));
        QVERIFY(touch(vault.filePath("b.wav")));
        QVERIFY(touch(vault.filePath("b.wav.md"), "[0] two\n"));
        QVERIFY(touch(vault.filePath("c.wav")));
        QVERIFY(touch(vault.filePath("c.wav.md"), "[0] three\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

        // Whole-file trash.
        QVERIFY(trashOne(model, QUrl::fromLocalFile(vault.filePath("a.wav"))));
        QCOMPARE(resetSpy.count(), 1); // the explicit applyRemovalOutcome rebuild
        QTest::qWait(300);             // let the watcher echo fire and be swallowed
        QCOMPARE(resetSpy.count(), 1);

        // Transcript-only trash (b.wav flips transcribed -> untranscribed):
        // status is content, not structure (item 038), so this refreshes the
        // row in place with dataChanged and fires NO model reset.
        QVERIFY(trashTranscriptOne(model, QUrl::fromLocalFile(vault.filePath("b.wav"))));
        QCOMPARE(resetSpy.count(), 1);
        QTest::qWait(300);
        QCOMPARE(resetSpy.count(), 1);

        // Audio-only trash (c.wav's .md survives as an orphaned .md row) IS
        // structural (isOrphanedMd flips), so it still resets exactly once.
        QVERIFY(trashAudioOnlyOne(model, QUrl::fromLocalFile(vault.filePath("c.wav"))));
        QCOMPARE(resetSpy.count(), 2);
        QTest::qWait(300);
        QCOMPARE(resetSpy.count(), 2);
    }

    // Queue item 021: a rename genuinely IS structural (the node's name/path
    // change, so sameStructure() fails), and unlike delete there is no explicit
    // synchronous rebuild — the WATCHER path is the authoritative one, and it
    // must reset exactly ONCE. This guards the direction the 021 fix could break:
    // Sidebar.onFinished now stands down for structural tasks, so if this
    // watcher-driven reset ever stopped firing the tree would never reflow at all.
    void rename_resetsTheModelOnlyOnce()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a.wav")));
        QVERIFY(touch(vault.filePath("a.wav.md"), "[0] one\n"));
        QVERIFY(touch(vault.filePath("b.wav")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

        // Rename the pair on disk behind the model's back — exactly what a
        // RenameFileTask does; only the watcher can notice it.
        QVERIFY(QFile::rename(vault.filePath("a.wav"), vault.filePath("z.wav")));
        QVERIFY(QFile::rename(vault.filePath("a.wav.md"), vault.filePath("z.wav.md")));

        // One reset, once the 50ms rebuild debounce coalesces both file events.
        QTRY_COMPARE_WITH_TIMEOUT(resetSpy.count(), 1, 3000);
        QTest::qWait(300); // a second, redundant reset would land in this window
        QCOMPARE(resetSpy.count(), 1);
    }

    // applyRenameOutcome() updates the tree IN PLACE — the renamed node's
    // name/path change and its row slides to the new alphabetical slot via
    // beginMoveRows/dataChanged, with ZERO model resets. And because the in-place
    // node then matches what a fresh scan produces, the watcher's directoryChanged
    // echo finds sameStructure() true and is swallowed — still zero resets. This
    // is what makes a rename show ONLY the name changing, no whole-tree flash.
    void applyRenameOutcome_updatesInPlaceWithoutReset()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a.wav")));
        QVERIFY(touch(vault.filePath("a.wav.md"), "[0] one\n"));
        QVERIFY(touch(vault.filePath("m.wav")));
        QVERIFY(touch(vault.filePath("z.wav")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QCOMPARE(model.rowCount(), 3);

        auto orderedNames = [&] {
            QStringList names;
            const int nameRole = roleByName(model, "name");
            for (int r = 0; r < model.rowCount(); ++r)
                names << model.data(model.index(r, 0), nameRole).toString();
            return names;
        };
        QCOMPARE(orderedNames(), (QStringList{"a.wav", "m.wav", "z.wav"}));

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        QSignalSpy movedSpy(&model, &QAbstractItemModel::rowsMoved);

        // Rename a.wav -> n.wav on disk (as RenameFileTask would), then apply the
        // in-place update the controller drives on success.
        QVERIFY(QFile::rename(vault.filePath("a.wav"), vault.filePath("n.wav")));
        QVERIFY(QFile::rename(vault.filePath("a.wav.md"), vault.filePath("n.wav.md")));
        model.applyRenameOutcome(QUrl::fromLocalFile(vault.filePath("a.wav")),
                                 QUrl::fromLocalFile(vault.filePath("n.wav")));

        // In place: no reset, the row moved (a -> n sorts after m), text updated.
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(movedSpy.count(), 1);
        QCOMPARE(orderedNames(), (QStringList{"m.wav", "n.wav", "z.wav"}));

        // The watcher echo of the same rename must be swallowed — sameStructure()
        // now holds because the in-place node matches the fresh scan.
        QTest::qWait(300);
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(orderedNames(), (QStringList{"m.wav", "n.wav", "z.wav"}));
    }

    // Undo of a delete resets the model exactly ONCE — the explicit rebuild in
    // applyRestoreOutcome(); the watcher echo of the restore is swallowed.
    void undoOfDelete_resetsTheModelOnlyOnce()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("gone.wav")));
        QVERIFY(touch(vault.filePath("gone.wav.md"), "[0] hi\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap trashResult = model.trashFiles({ QUrl::fromLocalFile(vault.filePath("gone.wav")) });
        QCOMPARE(trashResult.value(QStringLiteral("trashed")).toInt(), 1);
        QTest::qWait(300); // let the trash's own echo settle before we start counting

        // A transcribed file trashes as audio + its .md sidecar, so restore
        // brings back every "restorable" entry (2 here), in one rebuild.
        const QVariantList restorable = restorableOf(trashResult);
        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        QCOMPARE(model.restoreFromTrash(restorable), int(restorable.size()));
        QCOMPARE(resetSpy.count(), 1); // the explicit applyRestoreOutcome rebuild
        QTest::qWait(300);
        QCOMPARE(resetSpy.count(), 1);
        QVERIFY(findByName(model, "gone.wav").isValid()); // really came back
    }

    // A rescan that finds nothing changed on disk must NOT reset the model —
    // the structural compare short-circuits before beginResetModel().
    void unchangedRescan_doesNotResetTheModel()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("x.wav")));
        QVERIFY(touch(vault.filePath("x.wav.md"), "[0] hi\n"));
        QVERIFY(touch(vault.filePath("y.wav")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        model.refresh(); // rebuild() with nothing changed on disk
        QCOMPARE(resetSpy.count(), 0);
    }

    // A sidecar .md rewritten IN PLACE (a relabel — same file, new content) is a
    // content change, not a structural one: rebuild() emits dataChanged for that
    // row rather than resetting the whole model, and the new label is visible.
    void sidecarRewrittenInPlace_emitsDataChangedNotReset()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.wav")));
        QVERIFY(touch(vault.filePath("talk.wav.md"),
                      "---\nmodel: base\nlabel: Before\n---\n\n[0] hi\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const int labelRole = roleByName(model, "label");
        QVERIFY(labelRole >= 0);
        QCOMPARE(model.data(findByName(model, "talk.wav"), labelRole).toString(), QStringLiteral("Before"));

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);

        // In-place relabel with a bumped mtime (a same-mtime rewrite is
        // invisible to the model by design — see Node::mdModified).
        QVERIFY(touch(vault.filePath("talk.wav.md"),
                      "---\nmodel: base\nlabel: After\n---\n\n[0] hi\n"));
        QVERIFY(bumpMtime(vault.filePath("talk.wav.md")));

        model.refresh();

        QCOMPARE(resetSpy.count(), 0);
        QVERIFY(dataSpy.count() >= 1);
        QCOMPARE(model.data(findByName(model, "talk.wav"), labelRole).toString(), QStringLiteral("After"));
    }

    // --- frontmatterField() caching (perf fix: UI lag) -------------------------

    // frontmatterField() backs LabelRole/TagsRole/SpeakersRole and used to
    // re-open + re-scan the sidecar .md on EVERY data() call — three separate
    // file opens per row, inside VaultSearchProxyModel's O(n log n) sort,
    // which is invalidated on every search keystroke. It is now cached per
    // path, invalidated by mtime+size (same scheme as transcriptBody()).
    //
    // The cache must be completely invisible from the outside: this pins the
    // three roles' values across repeated reads (a stale cache would show up
    // as a wrong value on the SECOND read) and across an in-place edit of each
    // field. Reading all three roles also covers the one-parse-serves-every-key
    // change — they share a single cache entry now, so a bug where one key's
    // value leaked into another's lookup would surface here.
    void frontmatterRoles_areCached_butNeverReturnStaleValues()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.wav")));
        QVERIFY(touch(vault.filePath("talk.wav.md"),
                      "---\nmodel: base\nlabel: First Title\ntags: alpha, beta\n"
                      "speakers: Ann, Bob\n---\n\n[0] hi\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const int labelRole = roleByName(model, "label");
        const int tagsRole = roleByName(model, "tags");
        const int speakersRole = roleByName(model, "speakers");
        QVERIFY(labelRole >= 0 && tagsRole >= 0 && speakersRole >= 0);

        const QModelIndex idx = findByName(model, "talk.wav");
        QVERIFY(idx.isValid());

        // Read every role twice — the second read is served from the cache and
        // must agree with the first.
        for (int pass = 0; pass < 2; ++pass) {
            QCOMPARE(model.data(idx, labelRole).toString(), QStringLiteral("First Title"));
            QCOMPARE(model.data(idx, tagsRole).toString(), QStringLiteral("alpha, beta"));
            QCOMPARE(model.data(idx, speakersRole).toString(), QStringLiteral("Ann, Bob"));
        }

        // In-place rewrite changing ALL THREE fields at once — the next read
        // must see every one of them, not the cached entry.
        QVERIFY(touch(vault.filePath("talk.wav.md"),
                      "---\nmodel: base\nlabel: Second Title Entirely\ntags: gamma\n"
                      "speakers: Cleo, Dan, Eve\n---\n\n[0] hi\n"));

        const QModelIndex after = findByName(model, "talk.wav");
        QVERIFY(after.isValid());
        QCOMPARE(model.data(after, labelRole).toString(), QStringLiteral("Second Title Entirely"));
        QCOMPARE(model.data(after, tagsRole).toString(), QStringLiteral("gamma"));
        QCOMPARE(model.data(after, speakersRole).toString(), QStringLiteral("Cleo, Dan, Eve"));
    }

    // A key absent from the frontmatter reads as empty, and stays empty on the
    // cached second read — the cache stores the whole parsed map, so a missing
    // key must resolve to "not present", never to some other key's value or to
    // a re-parse attempt that finds nothing and mistakenly caches nothing.
    void frontmatterField_missingKey_isEmptyOnEveryRead()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("bare.wav")));
        // Has model:, but no label:/tags:/speakers: at all.
        QVERIFY(touch(vault.filePath("bare.wav.md"), "---\nmodel: base\n---\n\n[0] hi\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        const QModelIndex idx = findByName(model, "bare.wav");
        QVERIFY(idx.isValid());

        for (int pass = 0; pass < 2; ++pass) {
            QVERIFY(model.data(idx, roleByName(model, "label")).toString().isEmpty());
            QVERIFY(model.data(idx, roleByName(model, "tags")).toString().isEmpty());
            QVERIFY(model.data(idx, roleByName(model, "speakers")).toString().isEmpty());
        }

        // Adding the field later must still be picked up (the empty reads above
        // cached an entry for this file).
        QVERIFY(touch(vault.filePath("bare.wav.md"),
                      "---\nmodel: base\nlabel: Now I Have One\n---\n\n[0] hi\n"));
        QCOMPARE(model.data(findByName(model, "bare.wav"), roleByName(model, "label")).toString(),
                 QStringLiteral("Now I Have One"));
    }

    // --- refreshFile() (bug fix: label always delayed by one file) -------------

    // Bug report: an in-place rewrite of an ALREADY-EXISTING sidecar .md
    // (e.g. auto-label's writeLabelingFields()) never fires
    // QFileSystemWatcher::directoryChanged — that only fires on a
    // directory's file add/remove/rename, not on an existing file's content
    // changing — so nothing tells the tree view to re-query this row's
    // LabelRole. The value itself is never stale (frontmatterField()'s cache
    // is invalidated by the file's own mtime+size, which an in-place rewrite
    // changes, so a re-query always sees the new value regardless of what
    // triggered it), but without an explicit notification the view has no
    // reason to call data() again, so
    // the new label sits correctly on disk yet stays invisible until some
    // LATER, unrelated directory event (typically the next file's .md being
    // created) forces a full rebuild(). This reproduces exactly that gap:
    // after the in-place rewrite, no dataChanged() has fired at all.
    void inPlaceMdRewrite_emitsNoDataChanged_untilRefreshFileCalled()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("session.wav")));
        QVERIFY(touch(vault.filePath("session.wav.md"),
                      "---\nmodel: base\n---\n\n[00:00:00.000 - 00:00:01.000] hello\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QModelIndex before = findByName(model, "session.wav");
        QVERIFY(before.isValid());
        QVERIFY(model.data(before, roleByName(model, "label")).toString().isEmpty());

        QSignalSpy dataChangedSpy(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

        // Simulate what writeLabelingFields() does: rewrite the ALREADY-
        // EXISTING sidecar .md in place with the new label — no file is
        // added, removed, or renamed, so no directoryChanged fires.
        QVERIFY(touch(vault.filePath("session.wav.md"),
                      "---\nmodel: base\nlabel: My Auto Title\n---\n\n"
                      "[00:00:00.000 - 00:00:01.000] hello\n"));

        // Give the watcher a beat to (not) fire, proving this is a genuine
        // gap rather than a timing fluke.
        QTest::qWait(200);
        QCOMPARE(dataChangedSpy.count(), 0);
        QCOMPARE(resetSpy.count(), 0);

        // The label IS already correct if read directly — the row just was
        // never told to re-query it. (This read also covers frontmatterField()'s
        // cache invalidating correctly: the empty-label read at the top of this
        // test populated the cache entry, and this one must NOT return it.)
        const QModelIndex stillTheSameIndex = findByName(model, "session.wav");
        QCOMPARE(model.data(stillTheSameIndex, roleByName(model, "label")).toString(),
                 QStringLiteral("My Auto Title"));

        // The fix: TranscriptionController::finished(fileUrl, ok, error) is
        // expected to route into this call for every in-place rewrite (see
        // VaultTreeModel::refreshFile()'s doc comment) — proving the single-
        // row notification path itself works correctly here.
        model.refreshFile(QUrl::fromLocalFile(vault.filePath("session.wav")));

        QCOMPARE(dataChangedSpy.count(), 1);
        const QList<QVariant> args = dataChangedSpy.takeFirst();
        QCOMPARE(args.at(0).toModelIndex(), stillTheSameIndex);
        QCOMPARE(args.at(1).toModelIndex(), stillTheSameIndex);
    }

    // refreshFile() also works when passed the sidecar .md path directly
    // (a standalone orphaned .md node, or any caller that happens to have
    // the .md path rather than the audio path) — it locates whichever node
    // fileUrl's path actually IS.
    void refreshFile_locatesOrphanedMdNodeByItsOwnPath()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("stray-note.md"), "---\n---\n\noriginal content\n"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy dataChangedSpy(&model, &QAbstractItemModel::dataChanged);

        model.refreshFile(QUrl::fromLocalFile(vault.filePath("stray-note.md")));
        QCOMPARE(dataChangedSpy.count(), 1);
    }

    // A no-op (no crash, no signal) for a path that doesn't resolve to any
    // node currently in the tree — a stale finished() signal for a file
    // that's since been deleted/renamed, or one outside the vault entirely.
    void refreshFile_unknownPath_isInertNotFatal()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("known.mp3")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy dataChangedSpy(&model, &QAbstractItemModel::dataChanged);
        model.refreshFile(QUrl::fromLocalFile(vault.filePath("does-not-exist.mp3")));
        QCOMPARE(dataChangedSpy.count(), 0);

        VaultTreeModel noVaultModel;
        noVaultModel.refreshFile(QUrl::fromLocalFile(vault.filePath("known.mp3"))); // no crash
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

    // --- createFolder / deleteFolder / moveFile (Item 40: folder management) ---

    // createFolder makes a new subfolder inside a directory that's part of the
    // vault (QDir::mkpath); an empty folder stays hidden from the tree by the
    // no-audio-content pruning, so this checks the folder on disk directly.
    void createFolder_makesSubfolderInsideVault()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("song.mp3"))); // keeps a rebuild meaningful

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(model.createFolder(QUrl::fromLocalFile(vault.path()), QStringLiteral("Interviews")));
        QVERIFY(QFileInfo(vault.filePath("Interviews")).isDir());

        // A newly-created folder becomes a valid move destination immediately
        // (vaultFolders() walks the filesystem, not the pruned tree).
        QVERIFY(model.vaultFolders().contains(vault.filePath("Interviews")));
    }

    // Empty/whitespace names, names with a path separator, and "."/".." are
    // rejected without touching the filesystem.
    void createFolder_rejectsInvalidNames()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QUrl root = QUrl::fromLocalFile(vault.path());
        QVERIFY(!model.createFolder(root, QString()));
        QVERIFY(!model.createFolder(root, QStringLiteral("   ")));
        QVERIFY(!model.createFolder(root, QStringLiteral("a/b")));
        QVERIFY(!model.createFolder(root, QStringLiteral("a\\b")));
        QVERIFY(!model.createFolder(root, QStringLiteral("..")));
        QVERIFY(!model.createFolder(root, QStringLiteral(".")));

        // "a/b" must not have created either an "a" or a nested "b".
        QVERIFY(!QFileInfo::exists(vault.filePath("a")));
    }

    // Creating a folder whose name already exists (as a folder OR a file) is
    // refused rather than clobbering it.
    void createFolder_rejectsCollision()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath("existing")));
        QVERIFY(touch(vault.filePath("taken.mp3")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QUrl root = QUrl::fromLocalFile(vault.path());
        QVERIFY(!model.createFolder(root, QStringLiteral("existing")));
        QVERIFY(!model.createFolder(root, QStringLiteral("taken.mp3")));
    }

    // A parent directory outside the vault is refused (createFolder never
    // writes outside the open vault).
    void createFolder_rejectsParentOutsideVault()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(!model.createFolder(QUrl::fromLocalFile(outside.path()), QStringLiteral("sneaky")));
        QVERIFY(!QFileInfo::exists(outside.filePath("sneaky")));
    }

    // deleteFolder removes a subfolder and everything inside it from the vault
    // — to the TRASH, not permanently. A folder delete takes every transcript
    // underneath it, so it's the most destructive action in the tree and has to
    // stay recoverable, exactly like deleteFile()/deleteTranscript().
    void deleteFolder_trashesSubfolderAndContents()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("junk/a.mp3")));
        QVERIFY(touch(vault.filePath("junk/a.mp3.md")));
        QVERIFY(touch(vault.filePath("junk/nested/b.wav")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QVERIFY(findByName(model, "junk").isValid());

        QVERIFY(trashOne(model, QUrl::fromLocalFile(vault.filePath("junk"))));
        QVERIFY(!QFileInfo::exists(vault.filePath("junk")));
        QVERIFY(!findByName(model, "junk").isValid());

        // Recoverable: the whole tree is in the trash, contents intact — NOT
        // permanently removed.
        const QString trashed = trashedPath(QStringLiteral("junk"));
        QVERIFY(QFileInfo(trashed).isDir());
        QVERIFY(QFileInfo::exists(trashed + "/a.mp3"));
        QVERIFY(QFileInfo::exists(trashed + "/a.mp3.md"));
        QVERIFY(QFileInfo::exists(trashed + "/nested/b.wav"));
    }

    // The vault root itself is never deletable (guard), even though it IS
    // "inside the vault" — its contents must survive the refused call.
    void deleteFolder_refusesVaultRoot()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("keep.mp3")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(!trashOne(model, QUrl::fromLocalFile(vault.path())));
        QVERIFY(QFileInfo(vault.path()).isDir());
        QVERIFY(QFileInfo::exists(vault.filePath("keep.mp3")));
    }

    // A folder outside the vault (or a ".." escape) is never deletable.
    void deleteFolder_refusesPathOutsideVault()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        QVERIFY(QDir().mkpath(outside.filePath("victim")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(!trashOne(model, QUrl::fromLocalFile(outside.filePath("victim"))));
        QVERIFY(QFileInfo(outside.filePath("victim")).isDir());
    }

    // moveFile on an AUDIO file carries its sibling .md transcript and its
    // .<fileName>.partial.jsonl resume-state file into the destination folder,
    // as one atomic set — the audio/.md pair is never split across folders.
    void moveFile_audioCarriesMdAndPartialSidecars()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "transcript"));
        QVERIFY(touch(vault.filePath(".talk.mp3.partial.jsonl"), "resume"));
        QVERIFY(QDir().mkpath(vault.filePath("Archive")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(model.moveFile(QUrl::fromLocalFile(vault.filePath("talk.mp3")),
                               QUrl::fromLocalFile(vault.filePath("Archive"))));

        QVERIFY(QFileInfo::exists(vault.filePath("Archive/talk.mp3")));
        QVERIFY(QFileInfo::exists(vault.filePath("Archive/talk.mp3.md")));
        QVERIFY(QFileInfo::exists(vault.filePath("Archive/.talk.mp3.partial.jsonl")));
        QVERIFY(!QFileInfo::exists(vault.filePath("talk.mp3")));
        QVERIFY(!QFileInfo::exists(vault.filePath("talk.mp3.md")));
        QVERIFY(!QFileInfo::exists(vault.filePath(".talk.mp3.partial.jsonl")));

        // The moved audio is now transcribed (green) under Archive/.
        QCOMPARE(statusOf(model, "talk.mp3"), int(VaultTreeModel::TranscribedStatus));
    }

    // Moving a standalone orphaned .md (no audio) moves just the one file.
    void moveFile_orphanedMdMovesAlone()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("stray.md"), "note"));
        QVERIFY(QDir().mkpath(vault.filePath("Notes")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(model.moveFile(QUrl::fromLocalFile(vault.filePath("stray.md")),
                               QUrl::fromLocalFile(vault.filePath("Notes"))));
        QVERIFY(QFileInfo::exists(vault.filePath("Notes/stray.md")));
        QVERIFY(!QFileInfo::exists(vault.filePath("stray.md")));
    }

    // A move whose destination already has a same-named file is refused, and
    // nothing (not even the sidecars) is moved.
    void moveFile_rejectsCollision_leavingSourceIntact()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("clip.mp3"), "source"));
        QVERIFY(touch(vault.filePath("clip.mp3.md"), "source transcript"));
        QVERIFY(QDir().mkpath(vault.filePath("Dest")));
        QVERIFY(touch(vault.filePath("Dest/clip.mp3"), "already here"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(!model.moveFile(QUrl::fromLocalFile(vault.filePath("clip.mp3")),
                                QUrl::fromLocalFile(vault.filePath("Dest"))));

        // Source (and its sidecar) untouched; destination collision untouched.
        QVERIFY(QFileInfo::exists(vault.filePath("clip.mp3")));
        QVERIFY(QFileInfo::exists(vault.filePath("clip.mp3.md")));
        QVERIFY(!QFileInfo::exists(vault.filePath("Dest/clip.mp3.md")));
        QFile dest(vault.filePath("Dest/clip.mp3"));
        QVERIFY(dest.open(QIODevice::ReadOnly));
        QCOMPARE(dest.readAll(), QByteArray("already here"));
    }

    // A destination outside the vault is refused.
    void moveFile_rejectsDestinationOutsideVault()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("clip.mp3")));
        QTemporaryDir outside;
        QVERIFY(outside.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(!model.moveFile(QUrl::fromLocalFile(vault.filePath("clip.mp3")),
                                QUrl::fromLocalFile(outside.path())));
        QVERIFY(QFileInfo::exists(vault.filePath("clip.mp3")));
        QVERIFY(!QFileInfo::exists(outside.filePath("clip.mp3")));
    }

    // moveFolder relocates a directory (and its subtree) into another folder.
    void moveFolder_relocatesDirectoryIntoAnotherFolder()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("loose/a.mp3")));
        QVERIFY(QDir().mkpath(vault.filePath("Archive")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(model.moveFolder(QUrl::fromLocalFile(vault.filePath("loose")),
                                 QUrl::fromLocalFile(vault.filePath("Archive"))));
        QVERIFY(QFileInfo::exists(vault.filePath("Archive/loose/a.mp3")));
        QVERIFY(!QFileInfo::exists(vault.filePath("loose")));
    }

    // moveFolder refuses to move a folder into itself or its own descendant
    // (would orphan the subtree), and never moves the vault root.
    void moveFolder_refusesIntoOwnDescendantAndVaultRoot()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("parent/child/a.mp3")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        // Into its own descendant.
        QVERIFY(!model.moveFolder(QUrl::fromLocalFile(vault.filePath("parent")),
                                  QUrl::fromLocalFile(vault.filePath("parent/child"))));
        QVERIFY(QFileInfo::exists(vault.filePath("parent/child/a.mp3")));

        // The vault root itself.
        QVERIFY(!model.moveFolder(QUrl::fromLocalFile(vault.path()),
                                  QUrl::fromLocalFile(vault.filePath("parent"))));
    }

    // vaultFolders() lists the vault root plus every real subfolder (including
    // empty ones the tree prunes), and excludes the hidden .transcriber dir.
    void vaultFolders_listsRootAndAllRealSubfolders()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a/song.mp3")));
        QVERIFY(QDir().mkpath(vault.filePath("empty")));       // pruned from tree, still a dest
        QVERIFY(QDir().mkpath(vault.filePath(".transcriber"))); // hidden — must be excluded

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QStringList folders = model.vaultFolders();
        QVERIFY(folders.contains(vault.path()));
        QVERIFY(folders.contains(vault.filePath("a")));
        QVERIFY(folders.contains(vault.filePath("empty")));
        QVERIFY(!folders.contains(vault.filePath(".transcriber")));
    }

    // --- deleteTranscript / deleteFile (Item 2: delete transcript + file) -------
    // NOTE: these assert the source no longer exists AT ITS PATH IN THE VAULT —
    // the invariant that holds whether the delete trashed (recoverable, the
    // preferred path) or fell back to a permanent remove. The trash
    // DESTINATION is asserted where it's the point of the test, over the
    // redirected home trash initTestCase() sets up:
    // deleteFile_trashesTheFileItself() and deleteFolder_trashesSubfolder-
    // AndContents().

    // deleteTranscript removes an audio file's sibling .md (+ .partial.jsonl)
    // and leaves the audio in place, reverting the row to untranscribed.
    void deleteTranscript_removesMdAndPartial_keepsAudio()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "transcript"));
        QVERIFY(touch(vault.filePath(".talk.mp3.partial.jsonl"), "resume"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QCOMPARE(statusOf(model, "talk.mp3"), int(VaultTreeModel::TranscribedStatus));

        QSignalSpy spy(&model, &VaultTreeModel::transcriptDeleted);
        QVERIFY(trashTranscriptOne(model, QUrl::fromLocalFile(vault.filePath("talk.mp3"))));

        QVERIFY(QFileInfo::exists(vault.filePath("talk.mp3")));            // audio kept
        QVERIFY(!QFileInfo::exists(vault.filePath("talk.mp3.md")));        // transcript gone
        QVERIFY(!QFileInfo::exists(vault.filePath(".talk.mp3.partial.jsonl")));
        QCOMPARE(spy.count(), 1);
        // Reverted to untranscribed (yellow), the audio node still present.
        QCOMPARE(statusOf(model, "talk.mp3"), int(VaultTreeModel::UntranscribedStatus));
    }

    // deleteTranscript on an orphaned .md (no audio) removes that .md — it IS
    // the transcript.
    void deleteTranscript_onOrphanedMd_removesTheMd()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("stray.md"), "note"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QCOMPARE(statusOf(model, "stray.md"), int(VaultTreeModel::OrphanedMdStatus));

        QVERIFY(trashTranscriptOne(model, QUrl::fromLocalFile(vault.filePath("stray.md"))));
        QVERIFY(!QFileInfo::exists(vault.filePath("stray.md")));
        QVERIFY(!findByName(model, "stray.md").isValid());
    }

    // deleteTranscript on an audio file with NO sidecar is a no-op returning
    // false — nothing to delete, and the audio is untouched.
    void deleteTranscript_noSidecar_isNoOpFalse()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("bare.mp3"), "audio"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy spy(&model, &VaultTreeModel::transcriptDeleted);
        QVERIFY(!trashTranscriptOne(model, QUrl::fromLocalFile(vault.filePath("bare.mp3"))));
        QVERIFY(QFileInfo::exists(vault.filePath("bare.mp3")));
        QCOMPARE(spy.count(), 0);
    }

    // trashAudioOnly is the exact complement of trashTranscript: the audio and
    // its (now-meaningless) resume partial go, the .md STAYS. This is the whole
    // contract of the action, and the one thing trashFiles() must never do.
    void trashAudioOnly_removesAudioAndPartial_keepsTranscript()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "# transcript"));
        QVERIFY(touch(vault.filePath(".talk.mp3.partial.jsonl"), "resume"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QCOMPARE(statusOf(model, "talk.mp3"), int(VaultTreeModel::TranscribedStatus));

        QSignalSpy kept(&model, &VaultTreeModel::audioDeletedTranscriptKept);
        QSignalSpy gone(&model, &VaultTreeModel::fileDeleted);
        QVERIFY(trashAudioOnlyOne(model, QUrl::fromLocalFile(vault.filePath("talk.mp3"))));

        QVERIFY(!QFileInfo::exists(vault.filePath("talk.mp3")));           // audio gone
        QVERIFY2(QFileInfo::exists(vault.filePath("talk.mp3.md")),
                 "the transcript is the ONE thing this action must keep");
        QVERIFY2(!QFileInfo::exists(vault.filePath(".talk.mp3.partial.jsonl")),
                 "the partial is resume state for the audio — it follows the audio out");
        QCOMPARE(readAll(vault.filePath("talk.mp3.md")), QByteArray("# transcript")); // intact

        // The signal carries WHERE the transcript now lives, because that's the
        // one thing a listener can't derive from "the audio is gone".
        QCOMPARE(kept.count(), 1);
        QCOMPARE(kept.first().at(0).toUrl(), QUrl::fromLocalFile(vault.filePath("talk.mp3")));
        QCOMPARE(kept.first().at(1).toUrl(), QUrl::fromLocalFile(vault.filePath("talk.mp3.md")));
        // NOT fileDeleted, and not both: a listener obeying that would close the
        // view, throwing away the transcript this action exists to preserve.
        QCOMPARE(gone.count(), 0);
    }

    // With no transcript to survive, there's nothing to keep the view on, so
    // this degrades to an ordinary file delete — plain fileDeleted.
    void trashAudioOnly_withNoTranscript_emitsPlainFileDeleted()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("bare.mp3"), "audio"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy kept(&model, &VaultTreeModel::audioDeletedTranscriptKept);
        QSignalSpy gone(&model, &VaultTreeModel::fileDeleted);
        QVERIFY(trashAudioOnlyOne(model, QUrl::fromLocalFile(vault.filePath("bare.mp3"))));

        QVERIFY(!QFileInfo::exists(vault.filePath("bare.mp3")));
        QCOMPARE(kept.count(), 0);
        QCOMPARE(gone.count(), 1);
    }

    // The surviving .md isn't stranded: with no sibling audio, scanDirectory()
    // reclassifies it as an orphaned .md, so the row changes kind rather than
    // vanishing. That reclassification is what makes "keep the transcript" a
    // real vault state instead of a dangling file.
    void trashAudioOnly_leavesTheMdAsAnOrphanedMdRow()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "# transcript"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QVERIFY(!findByName(model, "talk.mp3.md").isValid()); // a sidecar, not its own row

        QVERIFY(trashAudioOnlyOne(model, QUrl::fromLocalFile(vault.filePath("talk.mp3"))));

        QVERIFY(!findByName(model, "talk.mp3").isValid());   // the audio row is gone
        QVERIFY(findByName(model, "talk.mp3.md").isValid()); // ...replaced by the .md's own row
        QCOMPARE(statusOf(model, "talk.mp3.md"), int(VaultTreeModel::OrphanedMdStatus));
    }

    // Recoverable, like every other trash path: Undo restores the audio, and
    // the .md that never left re-pairs with it — back to a transcribed row.
    void trashAudioOnly_isUndoable_backToATranscribedRow()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "# transcript"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashAudioOnly({ QUrl::fromLocalFile(vault.filePath("talk.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);
        QVERIFY(QFileInfo::exists(trashedPath(QStringLiteral("talk.mp3"))));
        // Only the audio is restorable — the .md was never trashed, so it must
        // not appear in the list (restoring it would fail: it's still there).
        QCOMPARE(restorableOf(result).size(), 1);
        QCOMPARE(statusOf(model, "talk.mp3.md"), int(VaultTreeModel::OrphanedMdStatus));

        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 1);
        QCOMPARE(readAll(vault.filePath("talk.mp3")), QByteArray("audio"));
        QCOMPARE(statusOf(model, "talk.mp3"), int(VaultTreeModel::TranscribedStatus));
    }

    // "Drop the audio, keep the transcript" has no referent for an orphaned .md
    // (there's no audio) — Invalid, and emphatically NOT a delete of the .md.
    void trashAudioOnly_onOrphanedMd_isInvalidAndTouchesNothing()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("stray.md"), "note"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashAudioOnly({ QUrl::fromLocalFile(vault.filePath("stray.md")) });
        QCOMPARE(result.value("trashed").toInt(), 0);
        QCOMPARE(result.value("invalid").toList().size(), 1);
        QVERIFY2(QFileInfo::exists(vault.filePath("stray.md")),
                 "an Invalid audio-only delete must never remove the .md instead");
    }

    // ---- Batch deletes -----------------------------------------------------
    //
    // trashTranscript()/trashAudioOnly() take a LIST, like trashFiles(): the
    // Sidebar's ribbon buttons, its Delete key and its context menu all act on
    // a whole selection, and a single row is just a one-element batch (see
    // VaultTreeModel.h's deletion block). The load-bearing part is that a batch
    // costs ONE rebuild, not one per entry — a rebuild rescans the vault
    // recursively and resets the model.

    void trashTranscript_batch_removesEveryTranscriptAndKeepsEveryAudio()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        for (const QString &stem : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }) {
            QVERIFY(touch(vault.filePath(stem + ".mp3"), "audio"));
            QVERIFY(touch(vault.filePath(stem + ".mp3.md"), "# t"));
        }

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashTranscript({ QUrl::fromLocalFile(vault.filePath("a.mp3")),
                                                           QUrl::fromLocalFile(vault.filePath("b.mp3")),
                                                           QUrl::fromLocalFile(vault.filePath("c.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 3);
        for (const QString &stem : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }) {
            QVERIFY2(!QFileInfo::exists(vault.filePath(stem + ".mp3.md")), "every transcript in the batch goes");
            QVERIFY2(QFileInfo::exists(vault.filePath(stem + ".mp3")), "every audio in the batch stays");
            QCOMPARE(statusOf(model, stem + ".mp3"), int(VaultTreeModel::UntranscribedStatus));
        }
    }

    // The whole reason these take a list: one user action, one rescan. A batch
    // transcript trash is status-only (item 038), so it refreshes the rows in
    // place with dataChanged and fires NO model reset — and certainly not one
    // reset per entry.
    void trashTranscript_batch_refreshesInPlace_notOncePerEntry()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        for (const QString &stem : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }) {
            QVERIFY(touch(vault.filePath(stem + ".mp3"), "audio"));
            QVERIFY(touch(vault.filePath(stem + ".mp3.md"), "# t"));
        }

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);
        model.trashTranscript({ QUrl::fromLocalFile(vault.filePath("a.mp3")),
                                QUrl::fromLocalFile(vault.filePath("b.mp3")),
                                QUrl::fromLocalFile(vault.filePath("c.mp3")) });
        QCOMPARE(resetSpy.count(), 0);
        for (const QString &stem : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") })
            QCOMPARE(statusOf(model, stem + ".mp3"), int(VaultTreeModel::UntranscribedStatus));
        QVERIFY2(dataChangedCarriedRole(dataSpy, int(VaultTreeModel::StatusRole)),
                 "a batch transcript trash must refresh status in place via dataChanged");
    }

    // A batch is a set of independent entries: one bad entry must not strand
    // the good ones, and must be reported as its own outcome.
    void trashTranscript_batch_skipsEntriesWithNoTranscript_andStillTrashesTheRest()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("has.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("has.mp3.md"), "# t"));
        QVERIFY(touch(vault.filePath("bare.mp3"), "audio")); // no .md

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashTranscript({ QUrl::fromLocalFile(vault.filePath("bare.mp3")),
                                                           QUrl::fromLocalFile(vault.filePath("has.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);
        QVERIFY2(!QFileInfo::exists(vault.filePath("has.mp3.md")),
                 "a no-op entry earlier in the batch must not stop a later real one");
        // Nothing was refused by the filesystem, so nothing may be offered up
        // for a permanent delete — "there was no transcript" is not "the trash
        // said no".
        QVERIFY(result.value("untrashable").toList().isEmpty());
    }

    void trashAudioOnly_batch_removesEveryAudioAndKeepsEveryTranscript()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        for (const QString &stem : { QStringLiteral("a"), QStringLiteral("b") }) {
            QVERIFY(touch(vault.filePath(stem + ".mp3"), "audio"));
            QVERIFY(touch(vault.filePath(stem + ".mp3.md"), "# t"));
        }

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        const QVariantMap result = model.trashAudioOnly({ QUrl::fromLocalFile(vault.filePath("a.mp3")),
                                                          QUrl::fromLocalFile(vault.filePath("b.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 2);
        QCOMPARE(resetSpy.count(), 1);
        for (const QString &stem : { QStringLiteral("a"), QStringLiteral("b") }) {
            QVERIFY(!QFileInfo::exists(vault.filePath(stem + ".mp3")));
            QVERIFY(QFileInfo::exists(vault.filePath(stem + ".mp3.md")));
            // The surviving .md changes KIND rather than vanishing.
            QCOMPARE(statusOf(model, stem + ".mp3.md"), int(VaultTreeModel::OrphanedMdStatus));
        }
    }

    // Each entry earns its own signal, decided by whether ITS transcript
    // survived — a batch mixing the two kinds must not pick one for the lot.
    void trashAudioOnly_batch_emitsThePerEntrySignalEachEntryEarned()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("kept.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("kept.mp3.md"), "# t")); // transcript survives
        QVERIFY(touch(vault.filePath("bare.mp3"), "audio"));  // nothing survives

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy keptSpy(&model, &VaultTreeModel::audioDeletedTranscriptKept);
        QSignalSpy goneSpy(&model, &VaultTreeModel::fileDeleted);
        model.trashAudioOnly({ QUrl::fromLocalFile(vault.filePath("kept.mp3")),
                               QUrl::fromLocalFile(vault.filePath("bare.mp3")) });

        QCOMPARE(keptSpy.count(), 1);
        QCOMPARE(keptSpy.at(0).at(0).toUrl(), QUrl::fromLocalFile(vault.filePath("kept.mp3")));
        QCOMPARE(goneSpy.count(), 1);
        QCOMPARE(goneSpy.at(0).at(0).toUrl(), QUrl::fromLocalFile(vault.filePath("bare.mp3")));
    }

    void deleteTranscriptPermanently_batch_reportsHowManyItRemoved()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("a.mp3.md"), "# t"));
        QVERIFY(touch(vault.filePath("bare.mp3"), "audio")); // nothing to remove

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QCOMPARE(model.deleteTranscriptPermanently({ QUrl::fromLocalFile(vault.filePath("a.mp3")),
                                                     QUrl::fromLocalFile(vault.filePath("bare.mp3")) }),
                 1);
        QVERIFY(!QFileInfo::exists(vault.filePath("a.mp3.md")));
        QVERIFY(QFileInfo::exists(vault.filePath("a.mp3")));
    }

    // The confirmed, irreversible counterpart keeps the same promise.
    void deleteAudioOnlyPermanently_removesAudioWithoutTrashing_keepsTranscript()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("gone.mp3"), "x"));
        QVERIFY(touch(vault.filePath("gone.mp3.md"), "# t"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy kept(&model, &VaultTreeModel::audioDeletedTranscriptKept);
        QVERIFY(deleteAudioOnlyPermanentlyOne(model, QUrl::fromLocalFile(vault.filePath("gone.mp3"))));
        QVERIFY(!QFileInfo::exists(vault.filePath("gone.mp3")));
        QVERIFY2(QFileInfo::exists(vault.filePath("gone.mp3.md")),
                 "even the permanent path keeps the transcript");
        QVERIFY2(!QFileInfo::exists(trashedPath(QStringLiteral("gone.mp3"))),
                 "deleteAudioOnlyPermanently must NOT route through the trash");
        // Same signal as the trash path: the view must follow the transcript
        // whichever way the audio went.
        QCOMPARE(kept.count(), 1);
        QCOMPARE(kept.first().at(1).toUrl(), QUrl::fromLocalFile(vault.filePath("gone.mp3.md")));
    }

    // deleteFile removes the audio AND all its sidecars (.md + .partial.jsonl).
    void deleteFile_removesAudioAndAllSidecars()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "transcript"));
        QVERIFY(touch(vault.filePath(".talk.mp3.partial.jsonl"), "resume"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy spy(&model, &VaultTreeModel::fileDeleted);
        QVERIFY(trashOne(model, QUrl::fromLocalFile(vault.filePath("talk.mp3"))));

        QVERIFY(!QFileInfo::exists(vault.filePath("talk.mp3")));
        QVERIFY(!QFileInfo::exists(vault.filePath("talk.mp3.md")));
        QVERIFY(!QFileInfo::exists(vault.filePath(".talk.mp3.partial.jsonl")));
        QCOMPARE(spy.count(), 1);
        QVERIFY(!findByName(model, "talk.mp3").isValid());
    }

    // "Removed from the vault" above is specifically a TRASH, not a permanent
    // delete: an accidental delete stays recoverable, with the file's contents
    // intact. Asserted over the redirected home trash (see initTestCase()).
    void deleteFile_trashesTheFileItself()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("precious.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("precious.mp3.md"), "transcript"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QVERIFY(trashOne(model, QUrl::fromLocalFile(vault.filePath("precious.mp3"))));

        QVERIFY(!QFileInfo::exists(vault.filePath("precious.mp3")));
        QVERIFY(QFileInfo::exists(trashedPath(QStringLiteral("precious.mp3"))));
        QVERIFY(QFileInfo::exists(trashedPath(QStringLiteral("precious.mp3.md"))));
        QFile recovered(trashedPath(QStringLiteral("precious.mp3.md")));
        QVERIFY(recovered.open(QIODevice::ReadOnly));
        QCOMPARE(recovered.readAll(), QByteArray("transcript"));  // intact, not a husk
    }

    // deleteFile on an orphaned .md removes just that one file.
    void deleteFile_onOrphanedMd_removesItAlone()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("stray.md"), "note"));
        QVERIFY(touch(vault.filePath("keep.mp3"), "audio")); // an unrelated file must survive

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(trashOne(model, QUrl::fromLocalFile(vault.filePath("stray.md"))));
        QVERIFY(!QFileInfo::exists(vault.filePath("stray.md")));
        QVERIFY(QFileInfo::exists(vault.filePath("keep.mp3")));
    }

    // Every delete refuses a path outside the vault (no escape), leaving the
    // external file untouched.
    void delete_refusesPathOutsideVault()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        QVERIFY(touch(outside.filePath("victim.mp3"), "audio"));
        QVERIFY(touch(outside.filePath("victim.mp3.md"), "transcript"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(!trashOne(model, QUrl::fromLocalFile(outside.filePath("victim.mp3"))));
        QVERIFY(!trashTranscriptOne(model, QUrl::fromLocalFile(outside.filePath("victim.mp3"))));
        QVERIFY(!trashAudioOnlyOne(model, QUrl::fromLocalFile(outside.filePath("victim.mp3"))));
        QVERIFY(!deleteAudioOnlyPermanentlyOne(model, QUrl::fromLocalFile(outside.filePath("victim.mp3"))));
        QVERIFY(QFileInfo::exists(outside.filePath("victim.mp3")));
        QVERIFY(QFileInfo::exists(outside.filePath("victim.mp3.md")));
    }

    // --- trashEntries (multi-select delete from the sidebar context menu) --------

    // The batch carries every selected file's sidecars off with it, exactly as
    // a per-file deleteFile() would, and reports how many it deleted.
    void deleteFiles_removesEverySelectedFileWithItsSidecars()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("a.mp3.md"), "transcript"));
        QVERIFY(touch(vault.filePath(".a.mp3.partial.jsonl"), "resume"));
        QVERIFY(touch(vault.filePath("b.wav"), "audio"));
        QVERIFY(touch(vault.filePath("stray.md"), "note"));
        QVERIFY(touch(vault.filePath("keep.mp3"), "audio")); // unselected — must survive

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QList<QUrl> selection = {
            QUrl::fromLocalFile(vault.filePath("a.mp3")),
            QUrl::fromLocalFile(vault.filePath("b.wav")),
            QUrl::fromLocalFile(vault.filePath("stray.md")),
        };
        QCOMPARE(trashMany(model, selection), 3);

        QVERIFY(!QFileInfo::exists(vault.filePath("a.mp3")));
        QVERIFY(!QFileInfo::exists(vault.filePath("a.mp3.md")));
        QVERIFY(!QFileInfo::exists(vault.filePath(".a.mp3.partial.jsonl")));
        QVERIFY(!QFileInfo::exists(vault.filePath("b.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("stray.md")));
        QVERIFY(QFileInfo::exists(vault.filePath("keep.mp3")));
        QVERIFY(!findByName(model, "a.mp3").isValid());
        QVERIFY(findByName(model, "keep.mp3").isValid());
    }

    // fileDeleted is per-file, not per-batch — MainPanel matches each emission
    // against the file it currently has open.
    void deleteFiles_emitsFileDeletedOncePerFile()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("b.mp3"), "audio"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy spy(&model, &VaultTreeModel::fileDeleted);
        QCOMPARE(trashMany(model, {QUrl::fromLocalFile(vault.filePath("a.mp3")),
                                    QUrl::fromLocalFile(vault.filePath("b.mp3"))}),
                 2);
        QCOMPARE(spy.count(), 2);
    }

    // One guard-failing entry (here: outside the vault, and a directory) must
    // not strand the valid files alongside it — they're deleted, it's skipped,
    // and the count reports the difference.
    void deleteFiles_skipsGuardFailuresWithoutAbortingTheBatch()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        QVERIFY(touch(outside.filePath("victim.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("good.mp3"), "audio"));
        QVERIFY(QDir().mkpath(vault.filePath("adir")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QList<QUrl> selection = {
            QUrl::fromLocalFile(outside.filePath("victim.mp3")), // outside the vault
            QUrl::fromLocalFile(vault.filePath("good.mp3")),
            QUrl::fromLocalFile(vault.filePath("adir")),         // a directory, never a file
            QUrl::fromLocalFile(vault.filePath("ghost.mp3")),    // doesn't exist
        };
        QCOMPARE(trashMany(model, selection), 1);

        QVERIFY(!QFileInfo::exists(vault.filePath("good.mp3")));
        QVERIFY(QFileInfo::exists(outside.filePath("victim.mp3")));
        QVERIFY(QFileInfo::exists(vault.filePath("adir")));
    }

    // --- restoreFromTrash (the snackbar's Undo) --------------------------------
    //
    // NOT covered here, and deliberately: the "untrashable" branch. Forcing
    // QFile::moveToTrash() to fail needs a filesystem whose mount top-dir the
    // user can't write (so no .Trash-<uid> can be created) AND an unwritable
    // XDG_DATA_HOME — which needs a real mount, i.e. root. Every path these
    // tests can reach trashes successfully. The non-destructive-on-failure
    // guarantee that the whole confirm-only-when-permanent flow rests on was
    // verified with a standalone probe instead (see trashOnly()'s comment).

    // The round trip the Undo action exists for: what came back must be the
    // same bytes, in the same place, sidecars included.
    void restoreFromTrash_bringsBackTheFileAndItsSidecars()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio-bytes"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "# transcript"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashFiles({ QUrl::fromLocalFile(vault.filePath("talk.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);
        QVERIFY(!QFileInfo::exists(vault.filePath("talk.mp3")));
        QVERIFY(!QFileInfo::exists(vault.filePath("talk.mp3.md")));

        // Audio + .md — the partial.jsonl never existed, so it isn't listed.
        const QVariantList restorable = restorableOf(result);
        QCOMPARE(restorable.size(), 2);

        QCOMPARE(model.restoreFromTrash(restorable), 2);
        QVERIFY(QFileInfo::exists(vault.filePath("talk.mp3")));
        QVERIFY(QFileInfo::exists(vault.filePath("talk.mp3.md")));
        QCOMPARE(readAll(vault.filePath("talk.mp3")), QByteArray("audio-bytes"));
        QCOMPARE(readAll(vault.filePath("talk.mp3.md")), QByteArray("# transcript"));
        // ...and the tree knows about it again.
        QCOMPARE(statusOf(model, "talk.mp3"), int(VaultTreeModel::TranscribedStatus));
    }

    // Qt has no restore API, so the .trashinfo record moveToTrash() wrote has to
    // be cleaned up by hand — leaving it orphans a phantom entry in the
    // desktop's trash UI pointing at a file that's no longer there.
    void restoreFromTrash_deletesTheOrphanedTrashInfoRecord()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("infoprobe.mp3"), "x"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashFiles({ QUrl::fromLocalFile(vault.filePath("infoprobe.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);

        // Derived from the trash path the result actually reports, NOT from the
        // original filename: the trash de-duplicates colliding names across
        // tests sharing one XDG_DATA_HOME, so a guessed path can silently point
        // at another test's leftover.
        const QString trashPath = restorableOf(result).first().toMap()
                                      .value("trashPath").toString();
        QVERIFY(!trashPath.isEmpty());
        const QString infoPath = QFileInfo(trashPath).absolutePath()
            + QStringLiteral("/../info/") + QFileInfo(trashPath).fileName()
            + QStringLiteral(".trashinfo");
        QVERIFY2(QFileInfo::exists(QDir::cleanPath(infoPath)),
                 "precondition: moveToTrash must write a .trashinfo record");

        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 1);
        QVERIFY(QFileInfo::exists(vault.filePath("infoprobe.mp3")));
        QVERIFY2(!QFileInfo::exists(QDir::cleanPath(infoPath)),
                 "restoring must delete the .trashinfo record, not orphan it");
    }

    // Never clobber. If the name got taken back while the snackbar was up (a
    // re-import, a rename), the restore must decline rather than overwrite.
    void restoreFromTrash_skipsAnOriginalPathThatIsOccupiedAgain()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("occupied.mp3"), "original"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashFiles({ QUrl::fromLocalFile(vault.filePath("occupied.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);

        // Something else takes the name back before Undo is pressed.
        QVERIFY(touch(vault.filePath("occupied.mp3"), "replacement"));

        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 0);
        QCOMPARE(readAll(vault.filePath("occupied.mp3")), QByteArray("replacement"));
    }

    // The trash can be emptied between the delete and the Undo — that's a
    // skipped entry, not a crash and not a bogus success.
    void restoreFromTrash_skipsWhenTheTrashedFileIsGone()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("emptied.mp3"), "x"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashFiles({ QUrl::fromLocalFile(vault.filePath("emptied.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);

        // Remove the file at the trash path the result reports (not a guessed
        // one — see the dedup note above), simulating an emptied trash.
        QVERIFY(QFile::remove(restorableOf(result).first().toMap()
                                  .value("trashPath").toString()));
        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 0);
        QVERIFY(!QFileInfo::exists(vault.filePath("emptied.mp3")));
    }

    // A restore must not become a way to write outside the vault, however the
    // entry list got here — same containment guard as the delete side.
    void restoreFromTrash_refusesAnOriginalPathOutsideTheVault()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        QVERIFY(touch(vault.filePath("escapee.mp3"), "x"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        const QVariantMap result = model.trashFiles({ QUrl::fromLocalFile(vault.filePath("escapee.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);

        // Hand-forge an entry aiming the restore out of the vault.
        QVariantList forged;
        forged.append(QVariantMap{
            { "trashPath", restorableOf(result).first().toMap().value("trashPath") },
            { "originalPath", outside.filePath("escaped.mp3") },
        });
        QCOMPARE(model.restoreFromTrash(forged), 0);
        QVERIFY(!QFileInfo::exists(outside.filePath("escaped.mp3")));
    }

    // Deleting a file whose folder is later removed, then undoing: the parent
    // has to come back or the rename fails.
    void restoreFromTrash_recreatesAMissingParentDirectory()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath("sub")));
        QVERIFY(touch(vault.filePath("sub/deep.mp3"), "x"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashFiles({ QUrl::fromLocalFile(vault.filePath("sub/deep.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);
        QVERIFY(QDir(vault.filePath("sub")).removeRecursively());
        QVERIFY(!QFileInfo::exists(vault.filePath("sub")));

        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 1);
        QVERIFY(QFileInfo::exists(vault.filePath("sub/deep.mp3")));
    }

    // A folder goes to the trash whole, and comes back whole.
    void restoreFromTrash_bringsBackAWholeFolderTree()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath("stuff")));
        QVERIFY(touch(vault.filePath("stuff/a.mp3"), "aaa"));
        QVERIFY(touch(vault.filePath("stuff/a.mp3.md"), "# a"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashFolder(QUrl::fromLocalFile(vault.filePath("stuff")));
        QCOMPARE(result.value("trashed").toInt(), 1);
        QVERIFY(!QFileInfo::exists(vault.filePath("stuff")));
        // One entry for the folder — its contents rode along inside it.
        QCOMPARE(restorableOf(result).size(), 1);

        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 1);
        QVERIFY(QFileInfo::exists(vault.filePath("stuff/a.mp3")));
        QCOMPARE(readAll(vault.filePath("stuff/a.mp3")), QByteArray("aaa"));
        QCOMPARE(readAll(vault.filePath("stuff/a.mp3.md")), QByteArray("# a"));
    }

    // Undoing a transcript delete brings the .md back — and with it the file's
    // green "transcribed" status.
    void restoreFromTrash_bringsBackATrashedTranscript()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "# transcript"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashTranscript({ QUrl::fromLocalFile(vault.filePath("talk.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);
        QCOMPARE(statusOf(model, "talk.mp3"), int(VaultTreeModel::UntranscribedStatus));

        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 1);
        QCOMPARE(readAll(vault.filePath("talk.mp3.md")), QByteArray("# transcript"));
        QCOMPARE(statusOf(model, "talk.mp3"), int(VaultTreeModel::TranscribedStatus));
    }

    // --- what a restore ANNOUNCES ---------------------------------------------
    //
    // rebuild() alone is enough for the tree (it re-reads everything) but not for
    // MainPanel, whose view state is its own: a delete drives the panel somewhere
    // via a signal, so an undo has to say enough for it to drive back. These
    // pin the inverse-signal contract the panel matches against.

    // The reported bug: audio-only delete moved the panel to the orphaned .md,
    // and undo left it stranded there because nothing said the audio was back.
    void restoreFromTrash_afterAudioOnlyDelete_announcesTheAudioAndItsTranscript()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "# transcript"));
        QVERIFY(touch(vault.filePath(".talk.mp3.partial.jsonl"), "resume"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashAudioOnly({ QUrl::fromLocalFile(vault.filePath("talk.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);

        QSignalSpy back(&model, &VaultTreeModel::audioRestoredForTranscript);
        QSignalSpy transcript(&model, &VaultTreeModel::transcriptRestored);
        QSignalSpy whole(&model, &VaultTreeModel::fileRestored);
        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 2); // audio + partial
        QCOMPARE(statusOf(model, "talk.mp3"), int(VaultTreeModel::TranscribedStatus));

        // Both urls, for the same reason audioDeletedTranscriptKept carries both:
        // the panel is sitting on the TRANSCRIPT, so that's what it matches on,
        // and the audio is where it has to go.
        QCOMPARE(back.count(), 1);
        QCOMPARE(back.first().at(0).toUrl(), QUrl::fromLocalFile(vault.filePath("talk.mp3")));
        QCOMPARE(back.first().at(1).toUrl(), QUrl::fromLocalFile(vault.filePath("talk.mp3.md")));
        // The .partial.jsonl came back in the same batch but is invisible to the
        // UI — announcing it would be noise, and it's no kind of transcript.
        QCOMPARE(transcript.count(), 0);
        // NOT fileRestored, and not both: the .md is absent from the batch
        // because it never left, which is exactly what says this was an
        // audio-only delete rather than a whole file coming back.
        QCOMPARE(whole.count(), 0);
    }

    // The same two paths, restored TOGETHER, mean the opposite thing — the batch
    // is the only thing that distinguishes them (see emitRestored()).
    void restoreFromTrash_afterAWholeFileDelete_announcesTheFileNotTheTranscript()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "# transcript"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashFiles({ QUrl::fromLocalFile(vault.filePath("talk.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);

        QSignalSpy whole(&model, &VaultTreeModel::fileRestored);
        QSignalSpy back(&model, &VaultTreeModel::audioRestoredForTranscript);
        QSignalSpy transcript(&model, &VaultTreeModel::transcriptRestored);
        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 2); // audio + .md

        // Once for the FILE, naming the audio — not once for the audio and again
        // for its transcript. Two rows came back on disk but only one in the
        // tree, and only one thing was ever closed.
        QCOMPARE(whole.count(), 1);
        QCOMPARE(whole.first().at(0).toUrl(), QUrl::fromLocalFile(vault.filePath("talk.mp3")));
        QCOMPARE(whole.first().at(1).toBool(), false); // an audio file is never an orphaned .md
        QCOMPARE(back.count(), 0);
        QCOMPARE(transcript.count(), 0);
    }

    // The inverse of transcriptDeleted, whose convention it follows: the url is
    // the AUDIO the transcript belongs to, not the .md that moved.
    void restoreFromTrash_afterTranscriptDelete_announcesTheAudioItBelongsTo()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "# transcript"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashTranscript({ QUrl::fromLocalFile(vault.filePath("talk.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);

        QSignalSpy transcript(&model, &VaultTreeModel::transcriptRestored);
        QSignalSpy back(&model, &VaultTreeModel::audioRestoredForTranscript);
        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 1);

        QCOMPARE(transcript.count(), 1);
        QCOMPARE(transcript.first().at(0).toUrl(), QUrl::fromLocalFile(vault.filePath("talk.mp3")));
        // The audio never went anywhere — nothing was restored FOR it.
        QCOMPARE(back.count(), 0);
    }

    // An orphaned .md is a file in its own right, not a transcript hanging off
    // one, so it comes back as fileRestored — flagged, because a listener
    // reopening it needs the text-only view, not a player.
    void restoreFromTrash_ofAnOrphanedMd_announcesItAsAnOrphanedFile()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("notes.md"), "# notes"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashFiles({ QUrl::fromLocalFile(vault.filePath("notes.md")) });
        QCOMPARE(result.value("trashed").toInt(), 1);

        QSignalSpy whole(&model, &VaultTreeModel::fileRestored);
        QSignalSpy transcript(&model, &VaultTreeModel::transcriptRestored);
        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 1);

        QCOMPARE(whole.count(), 1);
        QCOMPARE(whole.first().at(0).toUrl(), QUrl::fromLocalFile(vault.filePath("notes.md")));
        QCOMPARE(whole.first().at(1).toBool(), true);
        QCOMPARE(statusOf(model, "notes.md"), int(VaultTreeModel::OrphanedMdStatus));
        // It IS the file, not a transcript restored onto something else.
        QCOMPARE(transcript.count(), 0);
    }

    // An untranscribed audio went out as a plain fileDeleted, so it comes back as
    // a plain fileRestored — there's no transcript for it to re-adopt, and the
    // listener that closed on the delete is the one with something to do.
    void restoreFromTrash_ofAudioWithNoTranscript_announcesTheFile()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("bare.mp3"), "audio"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashFiles({ QUrl::fromLocalFile(vault.filePath("bare.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);

        QSignalSpy whole(&model, &VaultTreeModel::fileRestored);
        QSignalSpy back(&model, &VaultTreeModel::audioRestoredForTranscript);
        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 1);

        QCOMPARE(whole.count(), 1);
        QCOMPARE(whole.first().at(0).toUrl(), QUrl::fromLocalFile(vault.filePath("bare.mp3")));
        QCOMPARE(whole.first().at(1).toBool(), false);
        // Nothing survived to be restored FOR — that's what the other signal means.
        QCOMPARE(back.count(), 0);
    }

    // A restore that put nothing back must stay silent: the occupied-name and
    // emptied-trash guards above skip every entry, and a signal there would tell
    // MainPanel to open a file that isn't there.
    void restoreFromTrash_thatRestoredNothing_announcesNothing()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("talk.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("talk.mp3.md"), "# transcript"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap result = model.trashAudioOnly({ QUrl::fromLocalFile(vault.filePath("talk.mp3")) });
        QCOMPARE(result.value("trashed").toInt(), 1);
        // Something took the name back while the snackbar was up.
        QVERIFY(touch(vault.filePath("talk.mp3"), "a different file"));

        QSignalSpy back(&model, &VaultTreeModel::audioRestoredForTranscript);
        QCOMPARE(model.restoreFromTrash(restorableOf(result)), 0);
        QCOMPARE(back.count(), 0);
    }

    // The confirmed, irreversible path: no trash, nothing restorable.
    void deleteFilesPermanently_removesWithoutTrashing()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("gone.mp3"), "x"));
        QVERIFY(touch(vault.filePath("gone.mp3.md"), "# t"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QCOMPARE(model.deleteFilesPermanently({ QUrl::fromLocalFile(vault.filePath("gone.mp3")) }), 1);
        QVERIFY(!QFileInfo::exists(vault.filePath("gone.mp3")));
        QVERIFY2(!QFileInfo::exists(vault.filePath("gone.mp3.md")),
                 "a permanent delete must take the sidecars too");
        QVERIFY2(!QFileInfo::exists(trashedPath(QStringLiteral("gone.mp3"))),
                 "deleteFilesPermanently must NOT route through the trash");
    }

    // Same guards as the trash path — a permanent delete is the last place to
    // get containment wrong.
    void deleteFilesPermanently_keepsTheGuards()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        QVERIFY(touch(outside.filePath("victim.mp3"), "x"));
        QVERIFY(QDir().mkpath(vault.filePath("adir")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QCOMPARE(model.deleteFilesPermanently({
            QUrl::fromLocalFile(outside.filePath("victim.mp3")),  // outside the vault
            QUrl::fromLocalFile(vault.filePath("adir")),          // a directory, never a file
            QUrl::fromLocalFile(vault.path()),                    // the vault root itself
        }), 0);
        QVERIFY(QFileInfo::exists(outside.filePath("victim.mp3")));
        QVERIFY(QFileInfo::exists(vault.filePath("adir")));
        QVERIFY(QFileInfo::exists(vault.path()));
    }

    // An all-invalid batch is a no-op: nothing deleted, nothing emitted.
    void deleteFiles_emptyAndAllInvalidBatchesAreNoOps()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("keep.mp3"), "audio"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QSignalSpy spy(&model, &VaultTreeModel::fileDeleted);
        QCOMPARE(trashMany(model, {}), 0);
        QCOMPARE(trashMany(model, {QUrl::fromLocalFile(vault.filePath("ghost.mp3"))}), 0);
        QCOMPARE(spy.count(), 0);
        QVERIFY(QFileInfo::exists(vault.filePath("keep.mp3")));
    }

    // --- renameFolder (inline rename of a folder row) ---------------------------

    // The folder keeps its place and its contents; only the name changes.
    void renameFolder_renamesInPlaceKeepingContents()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("parent/old name/talk.mp3"), "audio"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(model.renameFolder(QUrl::fromLocalFile(vault.filePath("parent/old name")), "new name"));

        QVERIFY(!QFileInfo::exists(vault.filePath("parent/old name")));
        QVERIFY(QFileInfo::exists(vault.filePath("parent/new name/talk.mp3")));
        // Still inside the same parent — a rename never reparents.
        QVERIFY(findByName(model, "new name").isValid());
        QVERIFY(!findByName(model, "old name").isValid());
    }

    // The name is trimmed before use, matching createFolder().
    void renameFolder_trimsWhitespace()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath("interviews")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(model.renameFolder(QUrl::fromLocalFile(vault.filePath("interviews")), "  talks  "));
        QVERIFY(QFileInfo::exists(vault.filePath("talks")));
    }

    // Renaming to the name it already has is a no-op success, not a
    // self-collision failure.
    void renameFolder_sameNameIsANoOpSuccess()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("interviews/talk.mp3"), "audio"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(model.renameFolder(QUrl::fromLocalFile(vault.filePath("interviews")), "interviews"));
        QVERIFY(QFileInfo::exists(vault.filePath("interviews/talk.mp3")));
    }

    // Never clobbers an existing sibling folder.
    void renameFolder_refusesCollision()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a/keep.mp3"), "audio"));
        QVERIFY(touch(vault.filePath("b/other.mp3"), "audio"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QString error;
        QVERIFY(!model.renameFolder(QUrl::fromLocalFile(vault.filePath("a")), "b", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(QFileInfo::exists(vault.filePath("a/keep.mp3")));
        QVERIFY(QFileInfo::exists(vault.filePath("b/other.mp3")));
    }

    // Same guards as deleteFolder(): never the vault root, never outside it.
    void renameFolder_refusesVaultRootAndPathsOutsideVault()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        QVERIFY(QDir().mkpath(outside.filePath("victim")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QVERIFY(!model.renameFolder(QUrl::fromLocalFile(vault.path()), "hijacked"));
        QVERIFY(!model.renameFolder(QUrl::fromLocalFile(outside.filePath("victim")), "hijacked"));
        QVERIFY(QFileInfo::exists(outside.filePath("victim")));
        QVERIFY(!QFileInfo::exists(outside.filePath("hijacked")));
    }

    // A name can never carry a path separator (or other invalid character), so
    // a rename can't reparent or escape the vault via "../".
    void renameFolder_refusesInvalidNames()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("interviews/talk.mp3"), "audio"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QUrl folder = QUrl::fromLocalFile(vault.filePath("interviews"));
        QVERIFY(!model.renameFolder(folder, ""));
        QVERIFY(!model.renameFolder(folder, "   "));
        QVERIFY(!model.renameFolder(folder, "."));
        QVERIFY(!model.renameFolder(folder, ".."));
        QVERIFY(!model.renameFolder(folder, "../escaped"));
        QVERIFY(!model.renameFolder(folder, "sub/dir"));
        QVERIFY(!model.renameFolder(folder, "a*b"));

        // Nothing moved, nothing created.
        QVERIFY(QFileInfo::exists(vault.filePath("interviews/talk.mp3")));
        QVERIFY(!QFileInfo::exists(vault.filePath("escaped")));
    }

    // --- importDestinationFor (drag-and-drop import validation) ----------------
    //
    // Item 37: the drag-and-drop import copy is now an async worker-thread job
    // in TranscriptionController (see tst_transcription_queue's
    // importJob_* tests for the actual copy). VaultTreeModel now only owns the
    // cheap, synchronous VALIDATION half — returning the in-vault destination
    // path for a valid drop, or "" (with *errorOut set) for the fast failure
    // modes. These tests assert exactly that split: validation decides
    // accept/reject and computes the destination, but performs NO copy itself.

    // A recognized audio file validates: returns the vault-root destination
    // path, sets no error, and — being validation only — copies nothing (the
    // source is untouched and no file appears in the vault).
    void importDestinationFor_returnsVaultRootDest_forRecognizedAudio_withoutCopying()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QVERIFY(touch(source.filePath("dropped.mp3"), "audio bytes"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QString error;
        const QString dest =
            model.importDestinationFor(QUrl::fromLocalFile(source.filePath("dropped.mp3")), &error);
        QVERIFY2(!dest.isEmpty(), qPrintable(error));
        QVERIFY(error.isEmpty());
        QCOMPARE(QFileInfo(dest).absoluteFilePath(), QFileInfo(vault.filePath("dropped.mp3")).absoluteFilePath());

        // Validation must not have copied anything — that's the worker's job.
        QVERIFY(!QFileInfo::exists(vault.filePath("dropped.mp3")));
        QVERIFY(QFileInfo::exists(source.filePath("dropped.mp3"))); // source untouched
    }

    // A file with an unrecognized extension is refused (empty result + error),
    // not accepted as a destination.
    void importDestinationFor_rejectsNonAudioExtension()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QVERIFY(touch(source.filePath("notes.txt")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        QString error;
        QVERIFY(model.importDestinationFor(QUrl::fromLocalFile(source.filePath("notes.txt")), &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    // A same-named file already at the vault root is refused (never silently
    // overwritten) — the collision is caught synchronously, before enqueue.
    void importDestinationFor_rejectsCollisionWithExistingFile()
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
        QVERIFY(model.importDestinationFor(QUrl::fromLocalFile(source.filePath("song.mp3")), &error).isEmpty());
        QVERIFY(!error.isEmpty());

        QFile original(vault.filePath("song.mp3"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("original")); // untouched
    }

    // No vault open (empty vaultLocalPath): validation fails with an error
    // rather than pointing at some unintended location.
    void importDestinationFor_noVaultOpen_isInertNotFatal()
    {
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QVERIFY(touch(source.filePath("clip.mp3")));

        VaultTreeModel model;
        QString error;
        QVERIFY(model.importDestinationFor(QUrl::fromLocalFile(source.filePath("clip.mp3")), &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    // --- inspectImportCandidate (item 078: QML-readable per-candidate verdict) -
    //
    // importDestinationFor() collapses every failure to "" with the message in
    // a QString* QML can't read, so the drop handler can't distinguish a name
    // COLLISION (offer Overwrite/Skip) from a hard rejection.
    // inspectImportCandidate() is the new source of truth; importDestinationFor
    // now delegates to it. These tests assert the new verdict shape; the four
    // importDestinationFor_* tests above are the regression guard that the
    // delegation kept its existing contract (deliberately left unchanged).

    // A same-named file already at the vault root reports "collision" (not a
    // flat failure) with the would-be destination path and source name, so the
    // caller can offer Overwrite.
    void inspectImportCandidate_reportsCollision_withDest()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("song.mp3"), "original"));
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QVERIFY(touch(source.filePath("song.mp3"), "different bytes"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap v =
            model.inspectImportCandidate(QUrl::fromLocalFile(source.filePath("song.mp3")));
        QCOMPARE(v.value("reason").toString(), QStringLiteral("collision"));
        QVERIFY(v.value("dest").toString().endsWith(QStringLiteral("/song.mp3")));
        QCOMPARE(v.value("name").toString(), QStringLiteral("song.mp3"));
    }

    // A fresh, non-colliding audio file reports "ok" with a non-empty
    // destination.
    void inspectImportCandidate_reportsOk_withDest()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QVERIFY(touch(source.filePath("clip.mp3"), "audio bytes"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap v =
            model.inspectImportCandidate(QUrl::fromLocalFile(source.filePath("clip.mp3")));
        QCOMPARE(v.value("reason").toString(), QStringLiteral("ok"));
        QVERIFY(!v.value("dest").toString().isEmpty());
    }

    // An unrecognized extension reports "not-audio".
    void inspectImportCandidate_reportsNotAudio()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QVERIFY(touch(source.filePath("notes.txt")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantMap v =
            model.inspectImportCandidate(QUrl::fromLocalFile(source.filePath("notes.txt")));
        QCOMPARE(v.value("reason").toString(), QStringLiteral("not-audio"));
    }

    // No vault open reports "no-vault".
    void inspectImportCandidate_reportsNoVault()
    {
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QVERIFY(touch(source.filePath("clip.mp3")));

        VaultTreeModel model;
        const QVariantMap v =
            model.inspectImportCandidate(QUrl::fromLocalFile(source.filePath("clip.mp3")));
        QCOMPARE(v.value("reason").toString(), QStringLiteral("no-vault"));
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

    // Benchmark last-run cache round-trips through its own hidden JSON file and
    // is OVERWRITTEN (not appended) each write — only the most recent run is
    // kept, per the Benchmark page's spec.
    void benchmarkLastRun_roundTripsAndOverwrites()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        // Nothing written yet ⇒ empty map, not an error.
        QVERIFY(model.readBenchmarkLastRun().isEmpty());

        QVariantMap first;
        first[QStringLiteral("fileName")] = QStringLiteral("a.mp3");
        first[QStringLiteral("audioSeconds")] = 42.0;
        first[QStringLiteral("results")] = QVariantList{QVariantMap{{QStringLiteral("modelId"), QStringLiteral("base.en")}}};

        QString error;
        QVERIFY2(model.writeBenchmarkLastRun(first, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(vault.filePath(".transcriber/benchmarkLastRun.json")));

        QVariantMap readBack = model.readBenchmarkLastRun();
        QCOMPARE(readBack.value(QStringLiteral("fileName")).toString(), QStringLiteral("a.mp3"));
        QCOMPARE(readBack.value(QStringLiteral("audioSeconds")).toDouble(), 42.0);
        QCOMPARE(readBack.value(QStringLiteral("results")).toList().size(), 1);

        // A second write replaces the first entirely (overwrite, not append).
        QVariantMap second;
        second[QStringLiteral("fileName")] = QStringLiteral("b.wav");
        second[QStringLiteral("results")] = QVariantList{};
        QVERIFY(model.writeBenchmarkLastRun(second));

        readBack = model.readBenchmarkLastRun();
        QCOMPARE(readBack.value(QStringLiteral("fileName")).toString(), QStringLiteral("b.wav"));
        QVERIFY(!readBack.contains(QStringLiteral("audioSeconds")));
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

    // ---- Combine placeholders (setPendingCombines) -------------------------
    //
    // A combine job concatenates its sources into outputPath and only THEN
    // moves them into .combined/, so scanning the filesystem honestly shows the
    // merge not having happened right up until it has. setPendingCombines()
    // lets the tree show the merge as underway: sources out, one placeholder
    // in. See VaultTreeModel.h's doc comment.

    void setPendingCombines_hidesTheSources_andAddsOnePlaceholderInTheirPlace()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("part1.wav")));
        QVERIFY(touch(vault.filePath("part2.wav")));
        QVERIFY(touch(vault.filePath("unrelated.wav")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QVERIFY(findByName(model, "part1.wav").isValid());

        model.setPendingCombines({ QVariantMap{
            { "outputPath", vault.filePath("merged.wav") },
            { "sources", QStringList{ vault.filePath("part1.wav"), vault.filePath("part2.wav") } } } });

        QVERIFY2(!findByName(model, "part1.wav").isValid(), "a pending combine's sources leave the tree");
        QVERIFY2(!findByName(model, "part2.wav").isValid(), "a pending combine's sources leave the tree");
        QVERIFY2(findByName(model, "unrelated.wav").isValid(),
                 "a file no pending combine claims is untouched");

        // Exactly one placeholder, carrying the part count and the output path
        // (its identity — what lets a caller match it to the job driving it).
        const int placeholderRole = roleByName(model, "isCombinePlaceholder");
        QVERIFY(placeholderRole >= 0);
        QModelIndexList placeholders;
        for (int row = 0; row < model.rowCount(); ++row) {
            const QModelIndex idx = model.index(row, 0);
            if (model.data(idx, placeholderRole).toBool())
                placeholders << idx;
        }
        QCOMPARE(placeholders.size(), 1);
        QCOMPARE(model.data(placeholders.first(), roleByName(model, "combineSourceCount")).toInt(), 2);
        QCOMPARE(model.data(placeholders.first(), roleByName(model, "path")).toString(),
                 vault.filePath("merged.wav"));
    }

    // ffmpeg creates its output up front and grows it, so the merged path can
    // exist — half-written — for the whole run. It must not surface as ordinary
    // playable, transcribable audio; the placeholder stands there instead.
    void setPendingCombines_hidesTheHalfWrittenOutputFile_showingOnlyThePlaceholder()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("part1.wav")));
        QVERIFY(touch(vault.filePath("merged.wav"), "half-written")); // ffmpeg mid-run

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        model.setPendingCombines({ QVariantMap{
            { "outputPath", vault.filePath("merged.wav") },
            { "sources", QStringList{ vault.filePath("part1.wav") } } } });

        const int placeholderRole = roleByName(model, "isCombinePlaceholder");
        const int audioRole = roleByName(model, "isAudio");
        int audioRows = 0;
        int placeholderRows = 0;
        for (int row = 0; row < model.rowCount(); ++row) {
            const QModelIndex idx = model.index(row, 0);
            if (model.data(idx, audioRole).toBool())
                ++audioRows;
            if (model.data(idx, placeholderRole).toBool())
                ++placeholderRows;
        }
        QCOMPARE(placeholderRows, 1);
        QCOMPARE(audioRows, 0);
        QVERIFY2(!model.allAudioPaths().contains(vault.filePath("merged.wav")),
                 "a half-written combine output must never be offered as audio to transcribe");
    }

    // The placeholder is deliberately not a file row — that's what keeps every
    // existing "is this a note?" walk ignoring it without naming it.
    void setPendingCombines_placeholderIsNeitherAudioNorDirNorOrphanedMd()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("part1.wav")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        model.setPendingCombines({ QVariantMap{
            { "outputPath", vault.filePath("merged.wav") },
            { "sources", QStringList{ vault.filePath("part1.wav") } } } });

        const QModelIndex idx = model.index(0, 0);
        QVERIFY(model.data(idx, roleByName(model, "isCombinePlaceholder")).toBool());
        QVERIFY(!model.data(idx, roleByName(model, "isAudio")).toBool());
        QVERIFY(!model.data(idx, roleByName(model, "isDir")).toBool());
        QVERIFY(!model.data(idx, roleByName(model, "isOrphanedMd")).toBool());
        QVERIFY2(model.allAudioPaths().isEmpty(),
                 "a placeholder must never reach allAudioPaths() — bulk transcribe reads it");
        QVERIFY2(model.searchResults(QStringLiteral("merged"), false).isEmpty(),
                 "a placeholder names no file, so it can never be a search result");
    }

    // A hidden source's own .md must not pop out as a stray orphaned-.md row
    // just because its audio left the tree — the sidecar goes quiet with it.
    void setPendingCombines_hiddenSourceDoesNotStrandItsSidecarAsAnOrphanedMd()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("part1.wav")));
        QVERIFY(touch(vault.filePath("part1.wav.md"), "# t"));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        model.setPendingCombines({ QVariantMap{
            { "outputPath", vault.filePath("merged.wav") },
            { "sources", QStringList{ vault.filePath("part1.wav") } } } });

        QVERIFY(!findByName(model, "part1.wav").isValid());
        QVERIFY2(!findByName(model, "part1.wav.md").isValid(),
                 "hiding a source must not surface its sidecar as an orphaned .md");
    }

    // Placeholders retire by dropping out of the pushed list — that one path
    // covers success (merged file now on disk, sources gone), and failure or
    // cancel (sources never moved). The model doesn't need to know which.
    void setPendingCombines_clearing_returnsTheTreeToWhateverIsActuallyOnDisk()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("part1.wav")));
        QVERIFY(touch(vault.filePath("part2.wav")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        const QVariantList pending{ QVariantMap{
            { "outputPath", vault.filePath("merged.wav") },
            { "sources", QStringList{ vault.filePath("part1.wav"), vault.filePath("part2.wav") } } } };
        model.setPendingCombines(pending);
        QVERIFY(!findByName(model, "part1.wav").isValid());

        // The combine failed / was cancelled: no merged file, sources intact.
        model.setPendingCombines({});
        QVERIFY2(findByName(model, "part1.wav").isValid(), "a retired placeholder gives the sources back");
        QVERIFY(findByName(model, "part2.wav").isValid());
        const int placeholderRole = roleByName(model, "isCombinePlaceholder");
        for (int row = 0; row < model.rowCount(); ++row)
            QVERIFY(!model.data(model.index(row, 0), placeholderRole).toBool());
    }

    // Sidebar pushes this from onQueueChanged, which fires on every queue
    // transition — the overwhelming majority touching no combine at all. A
    // rebuild is a full recursive rescan + model reset, so an unchanged push
    // must cost nothing.
    void setPendingCombines_withAnUnchangedSet_doesNotRebuild()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("part1.wav")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        const QVariantList pending{ QVariantMap{
            { "outputPath", vault.filePath("merged.wav") },
            { "sources", QStringList{ vault.filePath("part1.wav") } } } };

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        model.setPendingCombines(pending);
        QCOMPARE(resetSpy.count(), 1); // the real change rebuilds
        model.setPendingCombines(pending);
        model.setPendingCombines(pending);
        QCOMPARE(resetSpy.count(), 1); // identical pushes are free
    }

    // A placeholder lands next to the files it is merging, not at the vault
    // root — the output path's own directory decides, same as any file row.
    void setPendingCombines_placesThePlaceholderInTheOutputPathsOwnFolder()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("sub/part1.wav")));
        QVERIFY(touch(vault.filePath("sub/part2.wav")));

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));
        model.setPendingCombines({ QVariantMap{
            { "outputPath", vault.filePath("sub/merged.wav") },
            { "sources", QStringList{ vault.filePath("sub/part1.wav"), vault.filePath("sub/part2.wav") } } } });

        const QModelIndex sub = findByName(model, "sub");
        QVERIFY(sub.isValid());
        const int placeholderRole = roleByName(model, "isCombinePlaceholder");
        int found = 0;
        for (int row = 0; row < model.rowCount(sub); ++row) {
            if (model.data(model.index(row, 0, sub), placeholderRole).toBool())
                ++found;
        }
        QCOMPARE(found, 1);
    }

    // Durable combine log (Issue 3): a merge is recorded to
    // .transcriber/combineHistory.json so an auto-combine that moved the user's
    // originals leaves an account of itself beyond the in-session Tasks row/undo.
    // Round-trips through the same array-of-entries store as the grab history,
    // and appends (never overwrites) so the log accumulates across merges.
    void combineHistory_appendsAndReadsBack()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        VaultTreeModel model;
        model.setVaultPath(QUrl::fromLocalFile(vault.path()));

        // Nothing logged yet: an empty list, not an error.
        QVERIFY(model.readCombineHistory().isEmpty());

        QVariantMap first{ { "outputPath", vault.filePath("Rec.combined.wav") },
                           { "sourcePaths", QStringList{ vault.filePath("REC001.wav"),
                                                         vault.filePath("REC002.wav") } },
                           { "partCount", 2 },
                           { "deleteOriginals", false } };
        QString err;
        QVERIFY2(model.appendCombineHistoryEntry(first, &err), qUtf8Printable(err));

        // The file lives under .transcriber/, alongside the grab history.
        QVERIFY(QFileInfo::exists(vault.filePath(".transcriber/combineHistory.json")));

        QVariantList history = model.readCombineHistory();
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().toMap().value("partCount").toInt(), 2);
        QCOMPARE(history.first().toMap().value("deleteOriginals").toBool(), false);

        // A second merge appends rather than replacing.
        QVariantMap second{ { "outputPath", vault.filePath("Other.combined.wav") },
                            { "partCount", 3 },
                            { "deleteOriginals", true } };
        QVERIFY(model.appendCombineHistoryEntry(second, nullptr));
        history = model.readCombineHistory();
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.at(1).toMap().value("deleteOriginals").toBool(), true);
    }
};

QTEST_GUILESS_MAIN(TstVaultTreeModel)
#include "tst_vaulttreemodel.moc"
