#include "audio_effect_processor.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QDebug>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutexLocker>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr qint64 OutputBufferUs = 120000;
constexpr qint64 PrebufferUs = 40000;
constexpr qint64 MaximumQueuedUs = 500000;
constexpr int FadeInMilliseconds = 10;
// The 38 ms feedback delay decays over several passes after the last input frame.
constexpr qint64 LiveTailUs = 200000;
constexpr qint64 DrainTimeoutMs = 5000;

float limited(float sample)
{
    if (!std::isfinite(sample)) {
        return 0.0F;
    }
    // 小信号保持线性，仅在峰值区域软限幅，且输出始终不超过满量程。
    constexpr float knee = 0.9F;
    const float magnitude = std::abs(sample);
    if (magnitude <= knee) {
        return sample;
    }
    const float compressed = knee + (1.0F - knee)
        * std::tanh((magnitude - knee) / (1.0F - knee));
    return std::copysign(std::min(compressed, 1.0F), sample);
}
}

AudioEffectProcessor::AudioEffectProcessor(QObject *parent)
    : QObject(parent)
    , m_flushTimer(new QTimer(this))
    , m_progressTimer(new QTimer(this))
{
    m_progressTimer->setInterval(20);
    m_progressTimer->setTimerType(Qt::PreciseTimer);
    connect(m_progressTimer, &QTimer::timeout, this, &AudioEffectProcessor::reportProgress);
    m_flushTimer->setTimerType(Qt::PreciseTimer);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(5);
    connect(m_flushTimer, &QTimer::timeout, this, &AudioEffectProcessor::flushOutput);
}

AudioEffectProcessor::~AudioEffectProcessor()
{
    stopOutput();
}

float AudioEffectProcessor::Biquad::process(float sample)
{
    const double output = b0 * sample + z1;
    z1 = b1 * sample - a1 * output + z2;
    z2 = b2 * sample - a2 * output;
    if (!std::isfinite(output) || !std::isfinite(z1) || !std::isfinite(z2)) {
        clear();
        return 0.0F;
    }
    return static_cast<float>(output);
}

void AudioEffectProcessor::Biquad::clear()
{
    z1 = 0.0;
    z2 = 0.0;
}

void AudioEffectProcessor::setPreset(Preset preset)
{
    const QMutexLocker locker(&m_dispatchMutex);
    if (m_requestedPreset == preset) {
        return;
    }
    m_requestedPreset = preset;
    QMetaObject::invokeMethod(this, [this, preset] {
        // Keep already processed PCM and the device buffer. New settings affect
        // subsequent input; switching effects is never a source reset.
        if (m_endOfStream || m_drainRequest != 0) {
            return;
        }
        m_preset = preset;
        for (ChannelState &channel : m_channels) {
            for (Biquad &filter : channel.filters) {
                filter.clear();
            }
        }
        std::fill(m_delayLine.begin(), m_delayLine.end(), 0.0F);
        m_delayPosition = 0;
        if (m_format.isValid()) {
            configureFilters();
        }
    }, Qt::QueuedConnection);
}

AudioEffectProcessor::Preset AudioEffectProcessor::preset() const
{
    const QMutexLocker locker(&m_dispatchMutex);
    return m_requestedPreset;
}

quint64 AudioEffectProcessor::generation() const
{
    return m_generation.load();
}

void AudioEffectProcessor::setVolume(qreal volume)
{
    const QMutexLocker locker(&m_dispatchMutex);
    QMetaObject::invokeMethod(this, [this, volume] {
        m_volume = std::clamp(volume, 0.0, 1.0);
        if (m_sink != nullptr) {
            m_sink->setVolume(m_volume);
        }
    }, Qt::QueuedConnection);
}

void AudioEffectProcessor::setPaused(bool paused)
{
    const QMutexLocker locker(&m_dispatchMutex);
    QMetaObject::invokeMethod(this, [this, paused] {
        if (paused && !m_paused && m_drainElapsed.isValid()) {
            m_drainActiveMs += m_drainElapsed.elapsed();
            m_drainElapsed.invalidate();
        } else if (!paused && m_paused && m_drainRequest != 0) {
            m_drainElapsed.start();
        }
        m_paused = paused;
        if (m_sink != nullptr) {
            if (paused) {
                m_flushTimer->stop();
                m_progressTimer->stop();
                m_sink->suspend();
                reportProgress();
            } else if (m_sink->state() == QtAudio::SuspendedState) {
                m_sink->resume();
                m_progressTimer->start();
            }
        }
        if (!paused) {
            flushOutput();
        }
    }, Qt::QueuedConnection);
}

void AudioEffectProcessor::reset()
{
    const QMutexLocker locker(&m_dispatchMutex);
    const quint64 nextGeneration = ++m_generation;
    const Preset nextPreset = m_requestedPreset;
    m_queuedInputUs = 0;
    m_inputOverflow = false;
    QMetaObject::invokeMethod(this, [this, nextGeneration, nextPreset] {
        stopOutput();
        m_outputGeneration = nextGeneration;
        m_preset = nextPreset;
        m_failed = false;
    }, Qt::QueuedConnection);
}

void AudioEffectProcessor::setStream(quint64 stream)
{
    const QMutexLocker locker(&m_dispatchMutex);
    m_stream = stream;
    const quint64 nextGeneration = ++m_generation;
    const Preset nextPreset = m_requestedPreset;
    m_queuedInputUs = 0;
    m_inputOverflow = false;
    QMetaObject::invokeMethod(this, [this, nextGeneration, nextPreset, stream] {
        stopOutput();
        m_outputGeneration = nextGeneration;
        m_outputStream = stream;
        m_preset = nextPreset;
        m_failed = false;
    }, Qt::QueuedConnection);
}

void AudioEffectProcessor::processBuffer(quint64 stream, const QAudioBuffer &buffer)
{
    const QMutexLocker locker(&m_dispatchMutex);
    if (stream == 0 || stream != m_stream || m_inputOverflow) {
        return;
    }
    const quint64 bufferGeneration = m_generation.load();
    const qint64 duration = qMax<qint64>(0, buffer.duration());
    // Bound the queue, but admit one codec frame even if its duration exceeds
    // the usual window (for example a large FLAC block at a low sample rate).
    const qint64 inputLimit = qMax(MaximumQueuedUs, duration);
    if (duration > inputLimit - m_queuedInputUs) {
        m_inputOverflow = true;
        QMetaObject::invokeMethod(this, [this, bufferGeneration] {
            if (bufferGeneration == m_generation.load() && !m_failed) {
                failOutput(QStringLiteral("音效输入积压超过允许时长"));
            }
        }, Qt::QueuedConnection);
        return;
    }
    m_queuedInputUs += duration;
    // QAudioBuffer 按值保留数据生命周期；后台入口不进行滤波或访问音频设备。
    QMetaObject::invokeMethod(this, [this, buffer, bufferGeneration, duration] {
        {
            const QMutexLocker inputLocker(&m_dispatchMutex);
            if (bufferGeneration != m_generation.load()) {
                return;
            }
        }
        processQueuedBuffer(buffer);
        {
            const QMutexLocker inputLocker(&m_dispatchMutex);
            // Include the buffer currently being processed in the drain count.
            // flushOutput() must not report completion before this buffer is appended.
            if (bufferGeneration == m_generation.load()) {
                m_queuedInputUs -= duration;
            }
        }
    }, Qt::QueuedConnection);
}

void AudioEffectProcessor::finishStream(quint64 request)
{
    const QMutexLocker locker(&m_dispatchMutex);
    const quint64 generation = m_generation.load();
    QMetaObject::invokeMethod(this, [this, request, generation] {
        if (generation != m_generation.load() || m_failed) {
            return;
        }
        if (m_drainRequest != request) {
            m_drainRequest = request;
            m_drainActiveMs = 0;
            m_drainElapsed.invalidate();
            if (!m_paused) {
                m_drainElapsed.start();
            }
        }
        m_endOfStream = true;
        // Preserve an explicit pause during EOF. Resuming restarts only the
        // active drain clock, so a long user pause cannot become an output error.
        if (!m_paused && m_sink != nullptr && m_sink->state() == QtAudio::SuspendedState) {
            m_sink->resume();
        }
        flushOutput();
    }, Qt::QueuedConnection);
}

void AudioEffectProcessor::processQueuedBuffer(const QAudioBuffer &buffer)
{
    if (m_failed || m_outputGeneration != m_generation.load()) {
        return;
    }
    if (!buffer.isValid() || buffer.byteCount() <= 0) {
        m_endOfStream = true;
        flushOutput();
        return;
    }
    if (m_endOfStream && m_drainRequest == 0) {
        m_tailAppended = false;
    }
    if (!ensureOutput(buffer.format())) {
        return;
    }
    flushOutput();
    if (m_outputDevice == nullptr) {
        return;
    }
    const int bytesPerFrame = m_format.bytesPerFrame();
    if (bytesPerFrame <= 0 || buffer.byteCount() % bytesPerFrame != 0) {
        failOutput(QStringLiteral("音效缓冲没有按完整音频帧对齐"));
        return;
    }

    // 先清理已写入前缀，短写留下的尾部必须和下一批数据连续输出。
    if (m_pendingOffset > 0) {
        m_pendingData.remove(0, m_pendingOffset);
        m_pendingOffset = 0;
    }
    const qint64 maximumBytes = qMax<qint64>(
        m_format.bytesForDuration(MaximumQueuedUs),
        buffer.byteCount() + m_format.bytesForDuration(OutputBufferUs + PrebufferUs));
    if (m_pendingData.size() + buffer.byteCount() > maximumBytes) {
        failOutput(QStringLiteral("音效输出积压超过允许时长"));
        return;
    }
    QByteArray processed(buffer.constData<char>(), buffer.byteCount());
    if (m_preset != Preset::Original) {
        processSamples(processed);
    }
    m_pendingData.append(processed);
    m_inputFrames += buffer.frameCount();
    m_endOfStream = m_drainRequest != 0;
    flushOutput();
}

bool AudioEffectProcessor::ensureOutput(const QAudioFormat &format)
{
    if (m_sink != nullptr && m_format == format) {
        return true;
    }
    stopOutput();
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (!format.isValid() || format.bytesPerFrame() <= 0
        || format.bytesPerSample() <= 0 || !device.isFormatSupported(format)) {
        failOutput(QStringLiteral("音效输出设备不支持当前音频格式"));
        return false;
    }
    m_format = format;
    m_channels.assign(static_cast<size_t>(format.channelCount()), ChannelState());
    const qsizetype delaySamples = qMax(
        1, qRound(format.sampleRate() * 0.038) * format.channelCount());
    m_delayLine.assign(static_cast<size_t>(delaySamples), 0.0F);
    configureFilters();
    m_fadeFrames = qMax(1, format.sampleRate() * FadeInMilliseconds / 1000);
    m_fadeFramesRemaining = m_fadeFrames;
    m_sink = new QAudioSink(device, format, this);
    m_sink->setBufferSize(qMax<qsizetype>(
        format.bytesForDuration(OutputBufferUs), format.bytesPerFrame() * 128));
    m_sink->setVolume(m_volume);
    m_outputDevice = m_sink->start();
    if (m_outputDevice == nullptr || m_sink->state() == QtAudio::StoppedState) {
        failOutput(QStringLiteral("无法启动音效输出设备"));
        return false;
    }
    if (m_paused) {
        m_sink->suspend();
    } else {
        m_progressTimer->start();
    }
    return true;
}

void AudioEffectProcessor::flushOutput()
{
    m_flushTimer->stop();
    if (m_paused || m_failed || m_outputGeneration != m_generation.load()) {
        return;
    }
    const qint64 drainMs = m_drainActiveMs +
        (m_drainElapsed.isValid() ? m_drainElapsed.elapsed() : 0);
    if (m_drainRequest != 0 && drainMs > DrainTimeoutMs) {
        failOutput(QStringLiteral("等待音效尾部输出超时，请检查音频设备后重试。"));
        return;
    }
    if (m_sink == nullptr || m_outputDevice == nullptr) {
        checkDrainCompletion();
        return;
    }
    if (m_sink->state() == QtAudio::StoppedState) {
        failOutput(QStringLiteral("音效输出设备已停止"));
        return;
    }
    if (m_sink->state() == QtAudio::IdleState && m_outputStarted) {
        // 欠载后重新积累一小段数据，避免每个短缓冲都触发一次断续播放。
        m_prebuffering = true;
        m_outputStarted = false;
        m_fadeFramesRemaining = m_fadeFrames;
    }
    const qsizetype pendingBytes = m_pendingData.size() - m_pendingOffset;
    if (pendingBytes <= 0) {
        checkDrainCompletion();
        return;
    }
    if (m_prebuffering && !m_endOfStream
        && pendingBytes < m_format.bytesForDuration(PrebufferUs)) {
        return;
    }
    m_prebuffering = false;

    while (m_pendingOffset < m_pendingData.size()) {
        const qint64 writable = qMin<qint64>(
            m_sink->bytesFree(), m_pendingData.size() - m_pendingOffset);
        if (writable <= 0) {
            break;
        }
        const qint64 written = m_outputDevice->write(
            m_pendingData.constData() + m_pendingOffset, writable);
        if (written < 0) {
            failOutput(QStringLiteral("音效数据写入失败"));
            return;
        }
        if (written == 0) {
            break;
        }
        m_pendingOffset += static_cast<qsizetype>(written);
        m_writtenBytes += written;
        m_outputStarted = true;
    }
    if (m_pendingOffset == m_pendingData.size()) {
        m_pendingData.clear();
        m_pendingOffset = 0;
        checkDrainCompletion();
    } else {
        // Only retry an actual short write. New input and end-of-stream already call this
        // method directly, so an empty/prebuffering queue needs no polling wakeups.
        m_flushTimer->start(5);
    }
}

void AudioEffectProcessor::checkDrainCompletion()
{
    if (m_drainRequest == 0 || m_paused || m_failed ||
        m_outputGeneration != m_generation.load()) {
        return;
    }
    {
        const QMutexLocker locker(&m_dispatchMutex);
        if (m_queuedInputUs > 0) {
            m_flushTimer->start(5);
            return;
        }
    }
    if (m_pendingOffset < m_pendingData.size()) {
        m_flushTimer->start(5);
        return;
    }
    if (!m_tailAppended && m_sink != nullptr && m_preset == Preset::Live) {
        m_tailAppended = true;
        // Feed silence through the existing filters/delay once, preserving the last echoes
        // without extending the input stream or growing buffers without a bound.
        QByteArray tail(m_format.bytesForDuration(LiveTailUs),
                        m_format.sampleFormat() == QAudioFormat::UInt8 ? char(128) : char(0));
        processSamples(tail);
        m_pendingData = std::move(tail);
        m_pendingOffset = 0;
        m_prebuffering = false;
        m_flushTimer->start(5);
        return;
    }
    // An empty application queue only means bytes were handed to QAudioSink. Wait for the
    // sink's own buffer to become idle before notifying the playlist.
    if (m_sink != nullptr && m_sink->state() != QtAudio::IdleState) {
        m_flushTimer->start(10);
        return;
    }
    reportProgress();
    m_progressTimer->stop();
    const quint64 request = m_drainRequest;
    m_drainRequest = 0;
    m_drainActiveMs = 0;
    m_drainElapsed.invalidate();
    emit outputDrained(request, m_outputGeneration);
}

void AudioEffectProcessor::reportProgress()
{
    if (m_sink == nullptr || m_failed || m_outputGeneration != m_generation.load() ||
        m_format.sampleRate() <= 0 || m_outputStream == 0) {
        return;
    }
    if (m_sink->state() == QtAudio::StoppedState) {
        failOutput(QStringLiteral("音频输出设备已停止"));
        return;
    }
    const qint64 processedUs = qMax<qint64>(0, m_sink->processedUSecs());
    const qint64 processedFrames = processedUs / 1000000 * m_format.sampleRate() +
        processedUs % 1000000 * m_format.sampleRate() / 1000000;
    const qint64 writtenFrames = m_writtenBytes / m_format.bytesPerFrame();
    // Idle confirms all written bytes were consumed. Tail frames never advance
    // the song position or grant decode credit beyond the actual input audio.
    const qint64 frames = qMin(m_inputFrames, m_sink->state() == QtAudio::IdleState
        ? writtenFrames : qMin(processedFrames, writtenFrames));
    if (frames > m_reportedFrames) {
        m_reportedFrames = frames;
        emit outputProgress(m_outputStream, frames);
    }
}

void AudioEffectProcessor::failOutput(const QString &reason)
{
    qWarning() << reason;
    m_failed = true;
    stopOutput();
    emit outputFailed(m_outputGeneration, reason);
}

void AudioEffectProcessor::configureFilters()
{
    // 为均衡器提升和现场混响预留峰值余量，避免增强后的采样持续触碰限幅。
    m_preGain = 0.64F;
    if (m_preset == Preset::BassBoost) {
        m_preGain = 0.5F;
    } else if (m_preset == Preset::Live) {
        m_preGain = 0.7F;
    }
    for (ChannelState &channel : m_channels) {
        channel.filters = {};
        if (m_preset == Preset::ClearVocal) {
            configureHighPass(channel.filters[0], 70.0, 0.707);
            configurePeaking(channel.filters[1], 260.0, -2.4, 0.9);
            configurePeaking(channel.filters[2], 2800.0, 3.2, 1.0);
        } else if (m_preset == Preset::BassBoost) {
            configureHighPass(channel.filters[0], 35.0, 0.707);
            configureLowShelf(channel.filters[1], 105.0, 5.0);
            configurePeaking(channel.filters[2], 280.0, -1.2, 0.9);
        } else if (m_preset == Preset::Live) {
            configureHighPass(channel.filters[0], 55.0, 0.707);
            configurePeaking(channel.filters[1], 1800.0, 1.4, 0.8);
            configurePeaking(channel.filters[2], 6500.0, 1.1, 0.8);
        }
    }
}

void AudioEffectProcessor::configurePeaking(Biquad &filter, double frequency,
                                             double gainDb, double quality) const
{
    const double amplitude = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * Pi * frequency / m_format.sampleRate();
    const double alpha = std::sin(omega) / (2.0 * quality);
    const double a0 = 1.0 + alpha / amplitude;
    filter.b0 = (1.0 + alpha * amplitude) / a0;
    filter.b1 = (-2.0 * std::cos(omega)) / a0;
    filter.b2 = (1.0 - alpha * amplitude) / a0;
    filter.a1 = (-2.0 * std::cos(omega)) / a0;
    filter.a2 = (1.0 - alpha / amplitude) / a0;
}

void AudioEffectProcessor::configureLowShelf(Biquad &filter, double frequency,
                                              double gainDb) const
{
    const double amplitude = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * Pi * frequency / m_format.sampleRate();
    const double cosine = std::cos(omega);
    const double sine = std::sin(omega);
    const double alpha = sine / std::sqrt(2.0);
    const double root = 2.0 * std::sqrt(amplitude) * alpha;
    const double a0 = (amplitude + 1.0) + (amplitude - 1.0) * cosine + root;
    filter.b0 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine + root) / a0;
    filter.b1 = 2.0 * amplitude * ((amplitude - 1.0) - (amplitude + 1.0) * cosine) / a0;
    filter.b2 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine - root) / a0;
    filter.a1 = -2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosine) / a0;
    filter.a2 = ((amplitude + 1.0) + (amplitude - 1.0) * cosine - root) / a0;
}

void AudioEffectProcessor::configureHighPass(Biquad &filter, double frequency,
                                              double quality) const
{
    const double omega = 2.0 * Pi * frequency / m_format.sampleRate();
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * quality);
    const double a0 = 1.0 + alpha;
    filter.b0 = ((1.0 + cosine) / 2.0) / a0;
    filter.b1 = (-(1.0 + cosine)) / a0;
    filter.b2 = filter.b0;
    filter.a1 = (-2.0 * cosine) / a0;
    filter.a2 = (1.0 - alpha) / a0;
}

float AudioEffectProcessor::processSample(float sample, int channel)
{
    float output = std::isfinite(sample) ? sample * m_preGain : 0.0F;
    for (Biquad &filter : m_channels.at(static_cast<size_t>(channel)).filters) {
        output = filter.process(output);
    }
    if (m_preset == Preset::Live && !m_delayLine.empty()) {
        const float delayed = m_delayLine.at(static_cast<size_t>(m_delayPosition));
        m_delayLine.at(static_cast<size_t>(m_delayPosition)) = output + delayed * 0.12F;
        m_delayPosition = (m_delayPosition + 1) % static_cast<qsizetype>(m_delayLine.size());
        output = output * 0.9F + delayed * 0.22F;
    }
    output = limited(output);
    if (m_fadeFramesRemaining > 0) {
        const float gain = static_cast<float>(m_fadeFrames - m_fadeFramesRemaining + 1)
            / static_cast<float>(m_fadeFrames);
        output *= gain;
        // 同一音频帧的各声道使用相同淡入增益，保持声道平衡。
        if (channel == m_format.channelCount() - 1) {
            --m_fadeFramesRemaining;
        }
    }
    return output;
}

void AudioEffectProcessor::processSamples(QByteArray &data)
{
    const int channels = m_format.channelCount();
    const int sampleCount = data.size() / m_format.bytesPerSample();
    if (m_format.sampleFormat() == QAudioFormat::Float) {
        auto *samples = reinterpret_cast<float *>(data.data());
        for (int index = 0; index < sampleCount; ++index) {
            samples[index] = processSample(samples[index], index % channels);
        }
    } else if (m_format.sampleFormat() == QAudioFormat::Int16) {
        auto *samples = reinterpret_cast<qint16 *>(data.data());
        for (int index = 0; index < sampleCount; ++index) {
            const float input = samples[index] / 32768.0F;
            samples[index] = static_cast<qint16>(std::clamp(
                processSample(input, index % channels), -1.0F, 1.0F) * 32767.0F);
        }
    } else if (m_format.sampleFormat() == QAudioFormat::Int32) {
        auto *samples = reinterpret_cast<qint32 *>(data.data());
        for (int index = 0; index < sampleCount; ++index) {
            const float input = static_cast<float>(samples[index] / 2147483648.0);
            const double output = processSample(input, index % channels);
            // float 无法精确表示 INT32_MAX，必须用 double 缩放并在转换前限制边界。
            const double scaled = std::clamp(
                output * 2147483648.0,
                static_cast<double>(std::numeric_limits<qint32>::min()),
                static_cast<double>(std::numeric_limits<qint32>::max()));
            samples[index] = static_cast<qint32>(scaled);
        }
    } else if (m_format.sampleFormat() == QAudioFormat::UInt8) {
        auto *samples = reinterpret_cast<quint8 *>(data.data());
        for (int index = 0; index < sampleCount; ++index) {
            const float input = (samples[index] - 128.0F) / 128.0F;
            const double output = processSample(input, index % channels);
            samples[index] = static_cast<quint8>(
                std::clamp(std::round(output * 128.0 + 128.0), 0.0, 255.0));
        }
    }
}

void AudioEffectProcessor::stopOutput()
{
    m_flushTimer->stop();
    m_progressTimer->stop();
    m_writtenBytes = 0;
    m_inputFrames = 0;
    m_reportedFrames = -1;
    m_drainRequest = 0;
    m_drainActiveMs = 0;
    m_tailAppended = false;
    m_drainElapsed.invalidate();
    m_outputDevice = nullptr;
    if (m_sink != nullptr) {
        // reset 立即丢弃旧音频，暂停后切歌或拖动进度时不能继续播放旧缓冲。
        m_sink->reset();
        delete m_sink;
        m_sink = nullptr;
    }
    m_pendingData.clear();
    m_pendingOffset = 0;
    m_prebuffering = true;
    m_outputStarted = false;
    m_endOfStream = false;
    m_fadeFramesRemaining = 0;
    m_delayPosition = 0;
    m_format = {};
    m_channels.clear();
    m_delayLine.clear();
}
