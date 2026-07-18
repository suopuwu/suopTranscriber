#pragma once

#include <QFileInfo>
#include <QSet>
#include <QString>

// The audio file extensions the vault recognizes as recordings. Header-only so
// both VaultTreeModel (tree building) and FileRename (the rename utility) share
// ONE list with no link dependency between them — a recording file's identity
// is a plain filename fact, not tree-model state.
inline const QSet<QString> &audioExtensions()
{
    static const QSet<QString> exts = {
        "mp3", "wav", "m4a", "flac", "ogg", "opus", "aac", "wma", "aiff", "alac",
    };
    return exts;
}

inline bool isAudioFile(const QFileInfo &info)
{
    return audioExtensions().contains(info.suffix().toLower());
}
