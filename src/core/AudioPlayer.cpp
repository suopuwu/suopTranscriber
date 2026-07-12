#include "AudioPlayer.h"

#include <QAudioDevice>

AudioPlayer::AudioPlayer(QObject *parent)
    : QObject(parent)
{
    m_player.setAudioOutput(&m_audioOutput);

    connect(&m_player, &QMediaPlayer::sourceChanged, this, &AudioPlayer::sourceChanged);
    connect(&m_player, &QMediaPlayer::positionChanged, this, &AudioPlayer::positionChanged);
    connect(&m_player, &QMediaPlayer::durationChanged, this, &AudioPlayer::durationChanged);
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, &AudioPlayer::playingChanged);

    // Follow the system default output device live (e.g. unplugging headphones
    // mid-playback) rather than only picking it up at the next play() call.
    connect(&m_devices, &QMediaDevices::audioOutputsChanged, this, [this] {
        m_audioOutput.setDevice(QMediaDevices::defaultAudioOutput());
    });
}

QUrl AudioPlayer::source() const
{
    return m_player.source();
}

void AudioPlayer::setSource(const QUrl &url)
{
    if (url == m_player.source())
        return;
    m_player.setSource(url);
}

bool AudioPlayer::isPlaying() const
{
    return m_player.playbackState() == QMediaPlayer::PlayingState;
}

qint64 AudioPlayer::position() const
{
    return m_player.position();
}

void AudioPlayer::setPosition(qint64 ms)
{
    m_player.setPosition(ms);
}

qint64 AudioPlayer::duration() const
{
    return m_player.duration();
}

void AudioPlayer::play()
{
    m_player.play();
}

void AudioPlayer::pause()
{
    m_player.pause();
}

void AudioPlayer::togglePlay()
{
    if (isPlaying())
        pause();
    else
        play();
}
