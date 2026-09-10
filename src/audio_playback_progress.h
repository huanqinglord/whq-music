#ifndef AUDIO_PLAYBACK_PROGRESS_H
#define AUDIO_PLAYBACK_PROGRESS_H

#include <QAudioBuffer>
#include <QElapsedTimer>
#include <QMutex>
#include <QString>

class AudioPlaybackProgress final
{
public:
    struct Snapshot
    {
        quint64 revision = 0;
        qint64 requestedStartUs = 0;
        qint64 firstStartUs = -1;
        qint64 lastEndUs = -1;
        qint64 frames = 0;
        int sampleRate = 0;
        qint64 ageMs = 0;
        bool ended = false;
        bool continuous = true;

        qint64 endTimeUs() const;
        QString description() const;
    };

    void reset(quint64 generation, qint64 positionMs);
    void invalidate();
    void receive(quint64 generation, const QAudioBuffer &buffer);
    Snapshot snapshot() const;

private:
    mutable QMutex m_mutex;
    quint64 m_generation = 0;
    Snapshot m_snapshot;
    QElapsedTimer m_lastBuffer;
};

#endif
