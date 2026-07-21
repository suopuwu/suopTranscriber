#include "UndoStack.h"

UndoStack::UndoStack(QObject *parent) : QObject(parent) {}

void UndoStack::push(const QVariantMap &entry)
{
    m_undo.append(entry);
    // A fresh user edit invalidates any pending redo history — once the user
    // diverges from the undone branch, that branch is no longer reachable.
    // (takeUndo()/takeRedo() move entries between the stacks directly, NOT
    // through here, so replaying history never triggers this clear.)
    m_redo.clear();
    emit canUndoChanged();
    emit canRedoChanged();
}

QVariantMap UndoStack::takeUndo()
{
    if (m_undo.isEmpty())
        return {};
    const QVariantMap entry = m_undo.takeLast().toMap();
    m_redo.append(entry);
    emit canUndoChanged();
    emit canRedoChanged();
    return entry;
}

QVariantMap UndoStack::takeRedo()
{
    if (m_redo.isEmpty())
        return {};
    const QVariantMap entry = m_redo.takeLast().toMap();
    m_undo.append(entry);
    emit canUndoChanged();
    emit canRedoChanged();
    return entry;
}

void UndoStack::clear()
{
    m_undo.clear();
    m_redo.clear();
    emit canUndoChanged();
    emit canRedoChanged();
}
