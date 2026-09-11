#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <QAudioBuffer>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMutex>
#include <QObject>
#include <QUrl>

#include <deque>
#include <memory>

class AudioDecoderWorker;
class QThread;
struct AudioDecoderControl;

// Local-file playback facade. Qt's enums remain the UI contract; decoding and
// pacing belong to our FFmpeg worker and the audible QAudioSink respectively.
class AudioPlayer final : public QObject
{
    Q_OBJECT

public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    QUrl source() const;
    qint64 position() const;
    qint64 duration() const;
    QMediaMetaData metaData() const;
    QMediaPlayer::PlaybackState playbackState() const;
    QMediaPlayer::MediaStatus mediaStatus() const;
    QMediaPlayer::Error error() const;
    bool isSeekable() const;

    void setSource(const QUrl &source);
    void setPosition(qint64 position);
    void setAudioStream(quint64 stream);
    void play();
    void pause();
    void stop();
    // 由输出线程直接反馈；仅更新受锁保护的传输状态，界面通知合并后投递。
    void reportOutputProgress(quint64 stream, qint64 processedFrames);

signals:
    void sourceChanged(const QUrl &source);
    void positionChanged(qint64 position);
    void durationChanged(qint64 duration);
    void metaDataChanged();
    void seekableChanged(bool seekable);
    void playbackStateChanged(QMediaPlayer::PlaybackState state);
    void mediaStatusChanged(QMediaPlayer::MediaStatus status);
    void errorOccurred(QMediaPlayer::Error error, const QString &description);
    // 从解码线程发出，直接连接的接收方必须支持跨线程入队，不能操作界面。
    void audioBufferReceived(quint64 stream, const QAudioBuffer &buffer);

private:
    struct AudioInterval
    {
        qint64 firstFrame = 0;
        qint64 frames = 0;
        qint64 sourceUs = 0;
    };

    void scheduleDecoder();
    void cancelDecoder(bool waitForClose);
    void setState(QMediaPlayer::PlaybackState state);
    void setStatus(QMediaPlayer::MediaStatus status);
    void receiveBuffer(quint64 serial, const QAudioBuffer &buffer);
    void publishOutputPosition();

    QThread *m_thread;
    AudioDecoderWorker *m_worker;
    std::shared_ptr<AudioDecoderControl> m_control;
    QUrl m_source;
    QMediaMetaData m_metadata;
    QMediaPlayer::PlaybackState m_state = QMediaPlayer::StoppedState;
    QMediaPlayer::PlaybackState m_requestedState = QMediaPlayer::StoppedState;
    QMediaPlayer::MediaStatus m_status = QMediaPlayer::NoMedia;
    QMediaPlayer::Error m_error = QMediaPlayer::NoError;
    qint64 m_position = 0;
    qint64 m_duration = 0;
    bool m_loaded = false;
    bool m_seekable = false;

    // 主线程的切流/取消、解码线程的入队和输出线程的反馈共用同一把短锁。
    // 持锁时只记账、投递事件；不执行设备操作、等待线程或更新界面。
    mutable QMutex m_transportMutex;
    quint64 m_stream = 0;
    quint64 m_outputSerial = 0;
    qint64 m_submittedFrames = 0;
    qint64 m_outputPosition = 0;
    int m_outputRate = 0;
    bool m_positionUpdatePending = false;
    std::deque<AudioInterval> m_intervals;
};

#endif
