#include "audio_player.h"
#include "audio_decoder_worker.h"

#include <QMediaDevices>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>

#include <algorithm>

AudioPlayer::AudioPlayer(QObject *parent)
    : QObject(parent)
    , m_thread(new QThread(this))
    , m_control(std::make_shared<AudioDecoderControl>())
{
    m_worker = new AudioDecoderWorker(m_control);
    m_worker->moveToThread(m_thread);
    m_thread->setObjectName(QStringLiteral("AudioDecoderThread"));
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &AudioDecoderWorker::loaded, this,
            [this](quint64 serial, qint64 duration, bool seekable,
                   const QMediaMetaData &metadata) {
                if (serial != m_control->serial.load()) {
                    return;
                }
                m_loaded = true;
                m_duration = qMax<qint64>(0, duration);
                m_metadata = metadata;
                m_seekable = seekable;
                emit durationChanged(m_duration);
                emit metaDataChanged();
                setStatus(QMediaPlayer::LoadedMedia);
                // Loading may synchronously trigger restoration/seek in the controller.
                if (serial != m_control->serial.load()) {
                    return;
                }
                emit seekableChanged(m_seekable);
                if (serial == m_control->serial.load()) {
                    setState(m_requestedState);
                }
            }, Qt::QueuedConnection);
    connect(m_worker, &AudioDecoderWorker::bufferReady, this,
            &AudioPlayer::receiveBuffer, Qt::DirectConnection);
    connect(m_worker, &AudioDecoderWorker::finished, this, [this](quint64 serial) {
        if (serial != m_control->serial.load()) {
            return;
        }
        m_control->playing.store(false);
        setState(QMediaPlayer::StoppedState);
        setStatus(QMediaPlayer::EndOfMedia);
    }, Qt::QueuedConnection);
    connect(m_worker, &AudioDecoderWorker::failed, this,
            [this](quint64 serial, QMediaPlayer::Error error, const QString &reason) {
                if (serial != m_control->serial.load()) {
                    return;
                }
                m_control->playing.store(false);
                m_error = error;
                setState(QMediaPlayer::StoppedState);
                setStatus(QMediaPlayer::InvalidMedia);
                emit errorOccurred(error, reason);
            }, Qt::QueuedConnection);
    m_thread->start();
}

AudioPlayer::~AudioPlayer()
{
    cancelDecoder(true);
    m_thread->quit();
    m_thread->wait();
}

QUrl AudioPlayer::source() const
{
    return m_source;
}

qint64 AudioPlayer::position() const
{
    const QMutexLocker locker(&m_transportMutex);
    if (m_stream != 0 && m_outputSerial == m_control->serial.load()) {
        return qMax(m_position, m_outputPosition);
    }
    return m_position;
}

qint64 AudioPlayer::duration() const
{
    return m_duration;
}

QMediaMetaData AudioPlayer::metaData() const
{
    return m_metadata;
}

QMediaPlayer::PlaybackState AudioPlayer::playbackState() const
{
    return m_state;
}

QMediaPlayer::MediaStatus AudioPlayer::mediaStatus() const
{
    return m_status;
}

QMediaPlayer::Error AudioPlayer::error() const
{
    return m_error;
}

bool AudioPlayer::isSeekable() const
{
    return m_seekable;
}

void AudioPlayer::setSource(const QUrl &source)
{
    cancelDecoder(true);
    m_source = source;
    m_metadata = {};
    m_position = 0;
    m_duration = 0;
    m_error = QMediaPlayer::NoError;
    m_requestedState = QMediaPlayer::StoppedState;
    m_loaded = false;
    m_seekable = false;
    setState(QMediaPlayer::StoppedState);
    emit sourceChanged(m_source);
    emit positionChanged(0);
    emit durationChanged(0);
    emit seekableChanged(false);
    if (source.isEmpty()) {
        setStatus(QMediaPlayer::NoMedia);
        return;
    }
    setStatus(QMediaPlayer::LoadingMedia);
    scheduleDecoder();
}

void AudioPlayer::setPosition(qint64 position)
{
    if (m_source.isEmpty()) {
        return;
    }
    cancelDecoder(false);
    m_position = qMax<qint64>(0, position);
    if (m_duration > 0) {
        m_position = qMin(m_position, m_duration);
    }
    m_error = QMediaPlayer::NoError;
    m_loaded = false;
    emit positionChanged(m_position);
    setStatus(QMediaPlayer::LoadingMedia);
    scheduleDecoder();
}

void AudioPlayer::setAudioStream(quint64 stream)
{
    {
        const QMutexLocker locker(&m_transportMutex);
        m_stream = stream;
        // 新流必须等 scheduleDecoder 绑定解码序号，禁止把旧缓冲标成新歌曲。
        m_outputSerial = 0;
    }
    if (stream == 0) {
        cancelDecoder(false);
    }
}

void AudioPlayer::play()
{
    if (m_source.isEmpty() || m_error != QMediaPlayer::NoError) {
        return;
    }
    m_requestedState = QMediaPlayer::PlayingState;
    m_control->playing.store(true);
    if (m_status == QMediaPlayer::EndOfMedia) {
        setPosition(0);
    } else if (!m_loaded && m_status != QMediaPlayer::LoadingMedia) {
        scheduleDecoder();
    }
    if (m_loaded) {
        setState(QMediaPlayer::PlayingState);
    }
    QMetaObject::invokeMethod(m_worker, &AudioDecoderWorker::resume, Qt::QueuedConnection);
}

void AudioPlayer::pause()
{
    if (m_source.isEmpty()) {
        return;
    }
    m_requestedState = QMediaPlayer::PausedState;
    m_control->playing.store(false);
    setState(QMediaPlayer::PausedState);
}

void AudioPlayer::stop()
{
    cancelDecoder(true);
    m_requestedState = QMediaPlayer::StoppedState;
    m_loaded = false;
    m_position = 0;
    setState(QMediaPlayer::StoppedState);
    emit positionChanged(0);
    setStatus(m_source.isEmpty() ? QMediaPlayer::NoMedia : QMediaPlayer::LoadedMedia);
}

void AudioPlayer::scheduleDecoder()
{
    setStatus(QMediaPlayer::LoadingMedia);
    const quint64 serial = m_control->serial.load();
    // Defer one GUI event turn to collect the controller's seek, stream ID and
    // pause/play commands as a single request before opening the worker source.
    QTimer::singleShot(0, this, [this, serial] {
        if (serial != m_control->serial.load() || m_source.isEmpty()) {
            return;
        }
        if (!m_source.isLocalFile()) {
            m_error = QMediaPlayer::ResourceError;
            setStatus(QMediaPlayer::InvalidMedia);
            emit errorOccurred(m_error, QStringLiteral("当前播放器只支持本地音频文件。"));
            return;
        }
        m_control->playing.store(m_requestedState == QMediaPlayer::PlayingState);
        const QString path = m_source.toLocalFile();
        const qint64 position = m_position;
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        {
            const QMutexLocker locker(&m_transportMutex);
            m_outputSerial = serial;
            m_outputPosition = position;
        }
        QMetaObject::invokeMethod(m_worker, [worker = m_worker, serial, path, position, device] {
            worker->open(serial, path, position, device);
        }, Qt::QueuedConnection);
    });
}

void AudioPlayer::cancelDecoder(bool waitForClose)
{
    {
        const QMutexLocker locker(&m_transportMutex);
        ++m_control->serial;
        m_control->playing.store(false);
        m_control->consumedFrames.store(0);
        m_outputSerial = 0;
        m_submittedFrames = 0;
        m_outputRate = 0;
        m_intervals.clear();
    }
    if (waitForClose) {
        // Metadata editing needs the file handle released, not merely paused.
        QMetaObject::invokeMethod(m_worker, &AudioDecoderWorker::close,
                                  Qt::BlockingQueuedConnection);
    }
}

void AudioPlayer::setState(QMediaPlayer::PlaybackState state)
{
    if (m_state != state) {
        m_state = state;
        emit playbackStateChanged(state);
    }
}

void AudioPlayer::setStatus(QMediaPlayer::MediaStatus status)
{
    if (m_status != status) {
        m_status = status;
        emit mediaStatusChanged(status);
    }
}

void AudioPlayer::receiveBuffer(quint64 serial, const QAudioBuffer &buffer)
{
    const QMutexLocker locker(&m_transportMutex);
    if (serial != m_control->serial.load() || serial != m_outputSerial || m_stream == 0) {
        return;
    }
    if (buffer.isValid() && buffer.frameCount() > 0) {
        m_outputRate = buffer.format().sampleRate();
        m_intervals.push_back({m_submittedFrames, buffer.frameCount(), buffer.startTime()});
        m_submittedFrames += buffer.frameCount();
    }
    // 校验、记账和投递不可被切流打断；下游直接槽只更新线程安全统计或入队。
    emit audioBufferReceived(m_stream, buffer);
}

void AudioPlayer::reportOutputProgress(quint64 stream, qint64 processedFrames)
{
    const QMutexLocker locker(&m_transportMutex);
    if (stream == 0 || stream != m_stream || m_outputRate <= 0 ||
        m_outputSerial != m_control->serial.load()) {
        return;
    }
    const qint64 frames = qBound<qint64>(0, processedFrames, m_submittedFrames);
    const qint64 consumed = qMax(frames, m_control->consumedFrames.load());
    m_control->consumedFrames.store(consumed);
    qint64 positionUs = m_outputPosition * 1000;
    while (!m_intervals.empty()) {
        const AudioInterval &interval = m_intervals.front();
        if (consumed <= interval.firstFrame) {
            break;
        }
        const qint64 played = qMin(consumed - interval.firstFrame, interval.frames);
        positionUs = interval.sourceUs + played * 1000000 / m_outputRate;
        if (played < interval.frames) {
            break;
        }
        m_intervals.pop_front();
    }
    const qint64 position = qMax(m_outputPosition, positionUs / 1000);
    if (position != m_outputPosition) {
        m_outputPosition = position;
        // 主线程繁忙期间最多保留一个通知，消费反馈仍实时推进，解码不会断粮。
        if (!m_positionUpdatePending) {
            m_positionUpdatePending = true;
            QMetaObject::invokeMethod(this, &AudioPlayer::publishOutputPosition, Qt::QueuedConnection);
        }
    }
}

void AudioPlayer::publishOutputPosition()
{
    qint64 position = 0;
    {
        const QMutexLocker locker(&m_transportMutex);
        m_positionUpdatePending = false;
        if (m_stream == 0 || m_outputSerial != m_control->serial.load()) {
            return;
        }
        position = m_outputPosition;
    }
    if (position != m_position) {
        m_position = position;
        emit positionChanged(position);
    }
}
