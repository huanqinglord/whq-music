#include "audio_playback_progress.h"

#include <QMutexLocker>

#include <limits>

namespace {
constexpr qint64 MicrosecondsPerSecond = 1000000;
}

qint64 AudioPlaybackProgress::Snapshot::endTimeUs() const
{
    if (lastEndUs >= 0) {
        return lastEndUs;
    }
    if (firstStartUs < 0 || sampleRate <= 0) {
        return -1;
    }
    return firstStartUs + frames / sampleRate * MicrosecondsPerSecond +
           frames % sampleRate * MicrosecondsPerSecond / sampleRate;
}

QString AudioPlaybackProgress::Snapshot::description() const
{
    return QStringLiteral("frames=%1 rate=%2 firstUs=%3 endUs=%4 eof=%5 continuous=%6")
        .arg(frames)
        .arg(sampleRate)
        .arg(firstStartUs)
        .arg(endTimeUs())
        .arg(ended)
        .arg(continuous);
}

void AudioPlaybackProgress::reset(quint64 generation, qint64 positionMs)
{
    const QMutexLocker locker(&m_mutex);
    m_generation = generation;
    m_snapshot = {};
    m_snapshot.requestedStartUs = qMax<qint64>(0, positionMs) * 1000;
    m_lastBuffer.start();
}

void AudioPlaybackProgress::invalidate()
{
    const QMutexLocker locker(&m_mutex);
    m_generation = 0;
}

void AudioPlaybackProgress::receive(quint64 generation, const QAudioBuffer &buffer)
{
    const QMutexLocker locker(&m_mutex);
    if (generation == 0 || generation != m_generation) {
        return;
    }
    if (!buffer.isValid() || buffer.frameCount() <= 0) {
        m_snapshot.ended = true;
        return;
    }
    const int rate = buffer.format().sampleRate();
    if (rate <= 0) {
        m_snapshot.continuous = false;
        return;
    }
    const qint64 startUs = buffer.startTime();
    if (m_snapshot.frames == 0) {
        m_snapshot.sampleRate = rate;
        // 已收到的有效 PCM 即使没有时间戳也参与进度统计；连续性仅用于诊断。
        // 缺少起始时间时以本次播放或定位请求为锚点，后续按采样数推进。
        m_snapshot.firstStartUs = startUs >= 0 ? startUs : m_snapshot.requestedStartUs;
        if (startUs < 0 || (m_snapshot.requestedStartUs == 0 && startUs > 1)) {
            m_snapshot.continuous = false;
        }
    } else {
        const qint64 expectedStartUs = m_snapshot.endTimeUs();
        if (startUs < 0 || rate != m_snapshot.sampleRate ||
            qAbs(startUs - expectedStartUs) > 2) {
            m_snapshot.continuous = false;
        }
    }
    if (buffer.frameCount() > std::numeric_limits<qint64>::max() - m_snapshot.frames) {
        m_snapshot.continuous = false;
        return;
    }
    m_snapshot.frames += buffer.frameCount();
    const qint64 effectiveStartUs = startUs >= 0 ? startUs
        : qMax(m_snapshot.requestedStartUs, m_snapshot.lastEndUs);
    m_snapshot.lastEndUs = effectiveStartUs + buffer.duration();
    m_snapshot.ended = false;
    ++m_snapshot.revision;
    m_lastBuffer.restart();
}

AudioPlaybackProgress::Snapshot AudioPlaybackProgress::snapshot() const
{
    const QMutexLocker locker(&m_mutex);
    Snapshot result = m_snapshot;
    result.ageMs = m_lastBuffer.isValid() ? m_lastBuffer.elapsed() : 0;
    return result;
}
