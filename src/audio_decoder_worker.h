#ifndef AUDIO_DECODER_WORKER_H
#define AUDIO_DECODER_WORKER_H

#include <QAudioBuffer>
#include <QAudioDevice>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QObject>

#include <atomic>
#include <memory>

class DecoderSession;
class QTimer;

struct AudioDecoderControl
{
    std::atomic<quint64> serial{0};
    std::atomic<bool> playing{false};
    std::atomic<qint64> consumedFrames{0};
};

class AudioDecoderWorker final : public QObject
{
    Q_OBJECT

public:
    explicit AudioDecoderWorker(std::shared_ptr<AudioDecoderControl> control);
    ~AudioDecoderWorker() override;
    void open(quint64 serial, const QString &path, qint64 position,
              const QAudioDevice &device);
    void close();
    void resume();

signals:
    void loaded(quint64 serial, qint64 duration, bool seekable,
                const QMediaMetaData &metadata);
    void bufferReady(quint64 serial, const QAudioBuffer &buffer);
    void finished(quint64 serial);
    void failed(quint64 serial, QMediaPlayer::Error error, const QString &reason);

private:
    void pump();

    std::shared_ptr<AudioDecoderControl> m_control;
    std::unique_ptr<DecoderSession> m_session;
    QTimer *m_timer;
    quint64 m_serial = 0;
    qint64 m_sentFrames = 0;
};

#endif
