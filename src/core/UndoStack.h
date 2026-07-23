#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

// The undo/redo STORAGE half lifted out of MainPanel.qml's pushUndo/undoLast/
// redoLast. It owns the two flat stacks of {type, ...} history
// entries and the push/pop/redo-invalidation semantics — nothing more. The
// APPLY step (MainPanel.applyHistoryEntry) is deliberately LEFT in QML: it is
// view-entangled (label/prelude/tags re-read frontmatter through the panel's
// text fields; speakerRename mutates the pendingSpeakerRenames optimistic
// overlay), so only the model-shaped storage moved here.
//
// Each entry records BOTH its pre-edit (oldValue) and post-edit (newValue)
// state so the SAME entry map can be replayed in either direction: takeUndo()
// pops the newest undo entry, moves it to the redo stack, and hands it back
// for the QML to apply "undo"; takeRedo() is the mirror. Entries are pushed
// BEFORE the mutation they guard. Both stacks reset on a file switch (clear())
// so Ctrl+Z/Ctrl+Y never reach across into a different note's history.
//
// WHY AN INSTANTIABLE QML_ELEMENT (not a QML_SINGLETON like TreeOrdering /
// TranscriptDisplayBuilder): an undo stack is per-panel STATE, not a stateless
// service. Each MainPanel owns its own `UndoStack { id: undoStack }` so two
// panels can never share one history. The sibling types are singletons only
// because they hold no state between calls; this one is nothing BUT state.
class UndoStack : public QObject {
    Q_OBJECT
    QML_ELEMENT

    // Replace the two `undoStack.length > 0` / `redoStack.length > 0` reads the
    // QML shortcuts used to gate on. NOTIFY fires on every mutation so the
    // `enabled:` bindings re-evaluate — the C++ equivalent of the JS
    // whole-array reassignment that used to trigger undoStackChanged().
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY canRedoChanged)

public:
    explicit UndoStack(QObject *parent = nullptr);

    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }

    // Record a new user edit. Appends to the undo stack and CLEARS the redo
    // stack — a fresh edit invalidates any pending redo branch (standard
    // undo/redo semantics; the user has diverged from the undone branch).
    Q_INVOKABLE void push(const QVariantMap &entry);

    // Pop the newest undo entry, move it onto the redo stack, and return it so
    // the caller can apply its "undo" side. Returns an EMPTY map when the undo
    // stack is empty — the caller checks that instead of a JS `undefined`, and
    // nothing is moved between stacks in that case.
    Q_INVOKABLE QVariantMap takeUndo();

    // Mirror of takeUndo(): pop the newest redo entry, move it back onto the
    // undo stack, and return it for the caller to apply its "redo" side.
    Q_INVOKABLE QVariantMap takeRedo();

    // Reset both stacks — called on a file switch so undo/redo is per-file.
    Q_INVOKABLE void clear();

signals:
    void canUndoChanged();
    void canRedoChanged();

private:
    QVariantList m_undo;
    QVariantList m_redo;
};
