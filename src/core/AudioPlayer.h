#pragma once

#include <QAudioOutput>
#include <QMediaPlayer>
#include <QObject>
#include <QUrl>
#include <qqml.h>

// Thin QML-facing wrapper around QMediaPlayer; plays the original file
// (never a resampled copy — that's only for transcription input).
class AudioPlayer : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(qint64 position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)

public:
    explicit AudioPlayer(QObject *parent = nullptr);

    QUrl source() const;
    void setSource(const QUrl &url);
    bool isPlaying() const;
    qint64 position() const;
    void setPosition(qint64 ms);
    qint64 duration() const;

public slots:
    void play();
    void pause();
    void togglePlay();

signals:
    void sourceChanged();
    void playingChanged();
    void positionChanged();
    void durationChanged();

private:
    QMediaPlayer m_player;
    QAudioOutput m_audioOutput;
};
