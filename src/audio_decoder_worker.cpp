#include "audio_decoder_worker.h"
#include "ffmpeg_api.h"

#include <QBuffer>
#include <QDebug>
#include <QElapsedTimer>
#include <QImage>
#include <QImageReader>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {
constexpr qint64 MicrosecondsPerSecond = 1000000;
constexpr qint64 DecodeAheadUs = 200000;
constexpr qint64 ChunkUs = 20000;
constexpr int MaximumConsecutiveErrors = 64;
constexpr int MaximumFrameBytes = 32 * 1024 * 1024;
constexpr int MaximumPacketBytes = 64 * 1024 * 1024;
constexpr qint64 IoTimeoutMs = 10000;
constexpr AVRational MicrosecondTimeBase{1, 1000000};

qint64 monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

AVSampleFormat sampleFormat(QAudioFormat::SampleFormat format)
{
    switch (format) {
    case QAudioFormat::UInt8:
        return AV_SAMPLE_FMT_U8;
    case QAudioFormat::Int16:
        return AV_SAMPLE_FMT_S16;
    case QAudioFormat::Int32:
        return AV_SAMPLE_FMT_S32;
    case QAudioFormat::Float:
        return AV_SAMPLE_FMT_FLT;
    default:
        return AV_SAMPLE_FMT_NONE;
    }
}
}

class DecoderSession final
{
public:
    enum class Result
    {
        Buffer,
        Yield,
        End,
        Failed
    };

    DecoderSession(std::shared_ptr<AudioDecoderControl> control, quint64 serial,
                   const QString &path, qint64 position)
        : m_api(FFmpegApi::instance())
        , m_control(std::move(control))
        , m_serial(serial)
        , m_path(path)
        , m_seekUs(position * 1000)
        , m_clockAnchorUs(m_seekUs)
    {
    }

    ~DecoderSession()
    {
        if (m_resampler != nullptr) {
            m_api.swr_free(&m_resampler);
        }
        if (m_api.available()) {
            m_api.av_channel_layout_uninit(&m_inputLayout);
            m_api.av_channel_layout_uninit(&m_outputLayout);
        }
        if (m_frame != nullptr) {
            m_api.av_frame_free(&m_frame);
        }
        if (m_packet != nullptr) {
            m_api.av_packet_free(&m_packet);
        }
        if (m_codec != nullptr) {
            m_api.avcodec_free_context(&m_codec);
        }
        if (m_format != nullptr) {
            m_api.avformat_close_input(&m_format);
        }
    }

    bool open(const QAudioDevice &device)
    {
        if (!m_api.available()) {
            m_error = m_api.errorString();
            return false;
        }
        if (device.isNull()) {
            m_error = QStringLiteral("未找到可用的音频输出设备。");
            m_errorCode = QMediaPlayer::ResourceError;
            return false;
        }
        m_format = m_api.avformat_alloc_context();
        if (m_format == nullptr) {
            return fatal(AVERROR(ENOMEM), QStringLiteral("创建音频读取器"));
        }
        m_format->interrupt_callback = {&DecoderSession::interrupt, this};
        m_format->probesize = 5 * 1024 * 1024;
        m_format->max_analyze_duration = 2 * MicrosecondsPerSecond;
        m_ioDeadline = monotonicMs() + IoTimeoutMs;
        const QByteArray path = m_path.toUtf8();
        int result = m_api.avformat_open_input(&m_format, path.constData(), nullptr, nullptr);
        if (result < 0) {
            m_errorCode = QMediaPlayer::ResourceError;
            return fatal(result, QStringLiteral("打开音频文件"));
        }
        m_ioDeadline = monotonicMs() + IoTimeoutMs;
        result = m_api.avformat_find_stream_info(m_format, nullptr);
        if (result < 0) {
            return fatal(result, QStringLiteral("读取音频格式"));
        }
        const AVCodec *decoder = nullptr;
        m_streamIndex = m_api.av_find_best_stream(
            m_format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
        if (m_streamIndex < 0 || decoder == nullptr) {
            return fatal(m_streamIndex, QStringLiteral("查找音频解码器"));
        }
        m_audioStream = m_format->streams[m_streamIndex];
        m_codec = m_api.avcodec_alloc_context3(decoder);
        if (m_codec == nullptr) {
            return fatal(AVERROR(ENOMEM), QStringLiteral("创建音频解码器"));
        }
        result = m_api.avcodec_parameters_to_context(m_codec, m_audioStream->codecpar);
        if (result < 0) {
            return fatal(result, QStringLiteral("读取音频参数"));
        }
        m_codec->pkt_timebase = m_audioStream->time_base;
        m_codec->thread_count = 1;
        m_codec->max_samples = MaximumFrameBytes / sizeof(float);
        // A local checksum/bitstream error is recoverable; do not enable EXPLODE.
        // Successfully decoded frames without timestamps remain usable audio.
        m_codec->err_recognition = AV_EF_CRCCHECK;
        result = m_api.avcodec_open2(m_codec, decoder, nullptr);
        if (result < 0) {
            return fatal(result, QStringLiteral("初始化音频解码器"));
        }
        m_packet = m_api.av_packet_alloc();
        m_frame = m_api.av_frame_alloc();
        if (m_packet == nullptr || m_frame == nullptr) {
            return fatal(AVERROR(ENOMEM), QStringLiteral("创建音频缓冲"));
        }
        if (m_audioStream->start_time != AV_NOPTS_VALUE) {
            m_originUs = timestampUs(m_audioStream->start_time);
        } else if (m_format->start_time != AV_NOPTS_VALUE) {
            m_originUs = m_format->start_time;
        }
        if (m_audioStream->duration != AV_NOPTS_VALUE && m_audioStream->duration > 0) {
            m_duration = timestampUs(m_audioStream->duration) / 1000;
        } else if (m_format->duration != AV_NOPTS_VALUE && m_format->duration > 0) {
            m_duration = m_format->duration / 1000;
        }
        m_seekable = m_format->pb != nullptr &&
            (m_format->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
        m_outputFormat = device.preferredFormat();
        QAudioFormat native = m_outputFormat;
        native.setSampleRate(m_codec->sample_rate);
        native.setSampleFormat(QAudioFormat::Float);
        const int channels = m_codec->ch_layout.nb_channels;
        if (channels == 1 || channels == 2) {
            native.setChannelConfig(
                QAudioFormat::defaultChannelConfigForChannelCount(channels));
        }
        if (native.isValid() && device.isFormatSupported(native)) {
            m_outputFormat = native;
        }
        if (!m_outputFormat.isValid() ||
            sampleFormat(m_outputFormat.sampleFormat()) == AV_SAMPLE_FMT_NONE) {
            m_error = QStringLiteral("音频设备不支持可用的 PCM 输出格式。");
            m_errorCode = QMediaPlayer::ResourceError;
            return false;
        }
        m_api.av_channel_layout_default(&m_outputLayout, m_outputFormat.channelCount());
        readMetadata();
        if (m_seekUs > 0) {
            if (!m_seekable) {
                m_error = QStringLiteral("当前音频文件不支持定位播放。");
                return false;
            }
            const qint64 timestamp = m_api.av_rescale_q(
                m_seekUs + m_originUs, MicrosecondTimeBase, m_audioStream->time_base);
            m_ioDeadline = monotonicMs() + IoTimeoutMs;
            result = m_api.av_seek_frame(m_format, m_streamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
            if (result < 0) {
                return fatal(result, QStringLiteral("定位音频"));
            }
            m_api.avcodec_flush_buffers(m_codec);
        }
        qInfo().noquote() << "Streaming decoder opened, path:" << m_path
                          << "codec:" << decoder->name
                          << "rate:" << m_outputFormat.sampleRate()
                          << "position(ms):" << m_seekUs / 1000;
        return true;
    }

    Result next(QAudioBuffer &buffer)
    {
        QElapsedTimer budget;
        budget.start();
        // Yield regularly so pause, seek and source replacement can cancel work.
        for (int step = 0; step < 64 && budget.elapsed() < 8; ++step) {
            if (m_control->serial.load() != m_serial || !m_control->playing.load()) {
                return Result::Yield;
            }
            if (m_pcmOffset < m_pcm.size()) {
                const int bytesPerFrame = m_outputFormat.bytesPerFrame();
                const qint64 available = (m_pcm.size() - m_pcmOffset) / bytesPerFrame;
                const qint64 count = qMin(available, qMax<qint64>(
                    1, m_outputFormat.framesForDuration(ChunkUs)));
                const qint64 startUs = m_pcmStartUs + framesUs(m_pcmOffset / bytesPerFrame);
                const qsizetype bytes = static_cast<qsizetype>(count * bytesPerFrame);
                buffer = QAudioBuffer(QByteArray(m_pcm.constData() + m_pcmOffset, bytes),
                                      m_outputFormat, startUs);
                m_pcmOffset += bytes;
                return Result::Buffer;
            }
            m_pcm.clear();
            m_pcmOffset = 0;
            if (m_framePending) {
                if (!convertFrame()) {
                    return Result::Failed;
                }
                continue;
            }
            const int received = m_api.avcodec_receive_frame(m_codec, m_frame);
            if (received == 0) {
                if ((m_frame->flags & AV_FRAME_FLAG_CORRUPT) != 0) {
                    m_api.av_frame_unref(m_frame);
                    if (!recoverable(QStringLiteral("损坏的音频帧"))) {
                        return Result::Failed;
                    }
                    continue;
                }
                m_framePending = true;
                continue;
            }
            if (received == AVERROR_EOF) {
                if (!m_demuxEnded || !m_drainSent) {
                    m_error = QStringLiteral("音频解码器提前停止，尚未读取到文件结束。");
                    return Result::Failed;
                }
                const int flushed = flushResampler();
                if (flushed < 0) {
                    return Result::Failed;
                }
                if (flushed > 0) {
                    continue;
                }
                if (!m_decodedAnyFrame) {
                    m_error = QStringLiteral("文件中没有可播放的音频数据。");
                    return Result::Failed;
                }
                qInfo().noquote() << "Streaming decoder ended, path:" << m_path
                                  << "skipped errors:" << m_warningCount
                                  << "repaired timestamps:" << m_timestampRepairs;
                return Result::End;
            }
            if (received == AVERROR_INVALIDDATA) {
                if (!recoverable(QStringLiteral("解码局部音频数据失败"))) {
                    return Result::Failed;
                }
                continue;
            }
            if (received != AVERROR(EAGAIN)) {
                fatal(received, QStringLiteral("解码音频"));
                return Result::Failed;
            }
            if (m_packetPending) {
                const int sent = m_api.avcodec_send_packet(m_codec, m_packet);
                if (sent == AVERROR(EAGAIN)) {
                    m_error = QStringLiteral("音频解码器无法继续接收或输出数据。");
                    return Result::Failed;
                }
                m_api.av_packet_unref(m_packet);
                m_packetPending = false;
                if (sent == AVERROR_INVALIDDATA) {
                    if (!recoverable(QStringLiteral("跳过无法解码的音频数据包"))) {
                        return Result::Failed;
                    }
                } else if (sent < 0) {
                    fatal(sent, QStringLiteral("提交音频数据"));
                    return Result::Failed;
                }
                continue;
            }
            if (m_demuxEnded) {
                if (m_drainSent) {
                    m_error = QStringLiteral("音频解码器未能完成尾部输出。");
                    return Result::Failed;
                }
                const int sent = m_api.avcodec_send_packet(m_codec, nullptr);
                if (sent < 0 && sent != AVERROR_EOF) {
                    fatal(sent, QStringLiteral("输出音频尾部"));
                    return Result::Failed;
                }
                m_drainSent = true;
                continue;
            }
            m_ioDeadline = monotonicMs() + IoTimeoutMs;
            const int read = m_api.av_read_frame(m_format, m_packet);
            if (read == AVERROR_EOF) {
                if (m_format->pb != nullptr && m_format->pb->error < 0 &&
                    m_format->pb->error != AVERROR_EOF) {
                    m_errorCode = QMediaPlayer::ResourceError;
                    fatal(m_format->pb->error, QStringLiteral("读取音频文件"));
                    return Result::Failed;
                }
                m_demuxEnded = true;
            } else if (read == AVERROR_INVALIDDATA) {
                m_api.av_packet_unref(m_packet);
                if (!recoverable(QStringLiteral("跳过无法解析的局部数据"))) {
                    return Result::Failed;
                }
            } else if (read < 0) {
                m_errorCode = QMediaPlayer::ResourceError;
                fatal(read, QStringLiteral("读取音频文件"));
                return Result::Failed;
            } else if (m_packet->stream_index != m_streamIndex) {
                m_api.av_packet_unref(m_packet);
            } else if (m_packet->size > MaximumPacketBytes) {
                m_error = QStringLiteral("音频数据包超过允许的内存上限。");
                return Result::Failed;
            } else {
                ++m_packetsWithoutFrame;
                if (m_packetsWithoutFrame > 1024) {
                    m_error = QStringLiteral("持续读取音频数据后仍无法解码出声音。");
                    return Result::Failed;
                }
                m_packetPending = true;
            }
        }
        return Result::Yield;
    }

    qint64 duration() const
    {
        return m_duration;
    }

    bool seekable() const
    {
        return m_seekable;
    }

    QMediaMetaData metadata() const
    {
        return m_metadata;
    }

    int outputRate() const
    {
        return m_outputFormat.sampleRate();
    }

    QString errorString() const
    {
        return m_error;
    }

    QMediaPlayer::Error errorCode() const
    {
        return m_errorCode;
    }

private:
    static int interrupt(void *opaque)
    {
        const auto *session = static_cast<DecoderSession *>(opaque);
        return session->m_control->serial.load() != session->m_serial ||
            monotonicMs() >= session->m_ioDeadline;
    }

    qint64 timestampUs(qint64 timestamp) const
    {
        return m_api.av_rescale_q(timestamp, m_audioStream->time_base, MicrosecondTimeBase);
    }

    qint64 framesUs(qint64 frames) const
    {
        return m_api.av_rescale_q(
            frames, AVRational{1, m_outputFormat.sampleRate()}, MicrosecondTimeBase);
    }

    qint64 nextSourceUs() const
    {
        return m_clockAnchorUs + framesUs(m_clockFrames);
    }

    bool fatal(int code, const QString &operation)
    {
        m_error = QStringLiteral("%1失败：%2").arg(operation, m_api.describeError(code));
        return false;
    }

    bool recoverable(const QString &reason)
    {
        ++m_warningCount;
        ++m_consecutiveErrors;
        m_fadeAfterError = true;
        if (m_warningCount <= 3) {
            qWarning().noquote() << "Streaming decoder recovered, path:" << m_path
                                 << "position(ms):" << nextSourceUs() / 1000
                                 << "reason:" << reason;
        }
        if (m_consecutiveErrors >= MaximumConsecutiveErrors) {
            m_error = QStringLiteral("连续损坏的数据过多，无法继续播放当前歌曲。");
            return false;
        }
        return true;
    }

    void readMetadata()
    {
        const auto tag = [this](const char *key) {
            const AVDictionaryEntry *entry =
                m_api.av_dict_get(m_audioStream->metadata, key, nullptr, 0);
            if (entry == nullptr) {
                entry = m_api.av_dict_get(m_format->metadata, key, nullptr, 0);
            }
            return entry != nullptr ? QString::fromUtf8(entry->value) : QString();
        };
        m_metadata.insert(QMediaMetaData::Title, tag("title"));
        m_metadata.insert(QMediaMetaData::AlbumTitle, tag("album"));
        const QString artist = tag("artist");
        if (!artist.isEmpty()) {
            m_metadata.insert(QMediaMetaData::ContributingArtist, QStringList{artist});
        }
        m_metadata.insert(QMediaMetaData::AlbumArtist, tag("album_artist"));
        m_metadata.insert(QMediaMetaData::Duration, m_duration);
        for (unsigned index = 0; index < m_format->nb_streams; ++index) {
            const AVStream *stream = m_format->streams[index];
            const AVPacket &picture = stream->attached_pic;
            if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0 ||
                picture.size <= 0 || picture.size > 8 * 1024 * 1024) {
                continue;
            }
            QByteArray data(reinterpret_cast<const char *>(picture.data), picture.size);
            QBuffer input(&data);
            input.open(QIODevice::ReadOnly);
            QImageReader reader(&input);
            const QSize size = reader.size();
            if (!size.isValid() || qint64(size.width()) * size.height() > 64000000) {
                continue;
            }
            reader.setScaledSize(size.scaled(1024, 1024, Qt::KeepAspectRatio));
            const QImage cover = reader.read();
            if (!cover.isNull()) {
                m_metadata.insert(QMediaMetaData::CoverArtImage, cover);
                break;
            }
        }
    }

    bool configureResampler()
    {
        const int rate = m_frame->sample_rate > 0
            ? m_frame->sample_rate : m_codec->sample_rate;
        const AVChannelLayout &layout = m_frame->ch_layout.nb_channels > 0
            ? m_frame->ch_layout : m_codec->ch_layout;
        if (rate <= 0 || layout.nb_channels <= 0 || layout.nb_channels > 64 ||
            m_frame->nb_samples <= 0) {
            m_error = QStringLiteral("解码后的音频参数无效。");
            return false;
        }
        m_api.av_channel_layout_uninit(&m_inputLayout);
        int result = m_api.av_channel_layout_copy(&m_inputLayout, &layout);
        if (result < 0) {
            return fatal(result, QStringLiteral("读取音频声道"));
        }
        if (m_inputLayout.order == AV_CHANNEL_ORDER_UNSPEC) {
            m_api.av_channel_layout_uninit(&m_inputLayout);
            m_api.av_channel_layout_default(&m_inputLayout, layout.nb_channels);
        }
        m_inputRate = rate;
        m_inputFormat = static_cast<AVSampleFormat>(m_frame->format);
        result = m_api.swr_alloc_set_opts2(
            &m_resampler, &m_outputLayout, sampleFormat(m_outputFormat.sampleFormat()),
            m_outputFormat.sampleRate(), &m_inputLayout, m_inputFormat, rate, 0, nullptr);
        if (result < 0) {
            return fatal(result, QStringLiteral("创建音频格式转换器"));
        }
        result = m_api.swr_init(m_resampler);
        if (result < 0) {
            return fatal(result, QStringLiteral("初始化音频格式转换器"));
        }
        return true;
    }

    bool convertFrame()
    {
        const int rate = m_frame->sample_rate > 0
            ? m_frame->sample_rate : m_codec->sample_rate;
        const AVChannelLayout &layout = m_frame->ch_layout.nb_channels > 0
            ? m_frame->ch_layout : m_codec->ch_layout;
        const bool layoutChanged = layout.order != AV_CHANNEL_ORDER_UNSPEC &&
            m_api.av_channel_layout_compare(&m_inputLayout, &layout) != 0;
        if (m_resampler != nullptr &&
            (rate != m_inputRate || m_frame->format != m_inputFormat || layoutChanged ||
             layout.nb_channels != m_inputLayout.nb_channels)) {
            // Drain the old conversion delay before accepting a midstream format change.
            const int flushed = flushResampler();
            if (flushed < 0) {
                return false;
            }
            if (flushed > 0) {
                return true;
            }
            m_api.swr_free(&m_resampler);
        }
        if (m_resampler == nullptr && !configureResampler()) {
            return false;
        }
        const int capacity = m_api.swr_get_out_samples(m_resampler, m_frame->nb_samples);
        if (capacity < 0 || capacity > MaximumFrameBytes / m_outputFormat.bytesPerFrame()) {
            m_error = QStringLiteral("解码后的音频帧超过允许的内存上限。");
            return false;
        }
        qint64 startUs = nextSourceUs();
        const qint64 timestamp = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
            ? m_frame->best_effort_timestamp : m_frame->pts;
        if (timestamp == AV_NOPTS_VALUE) {
            ++m_timestampRepairs;
        } else {
            const qint64 delayUs = m_api.av_rescale_q(
                m_api.swr_get_delay(m_resampler, m_inputRate),
                AVRational{1, m_inputRate}, MicrosecondTimeBase);
            const qint64 actualUs = timestampUs(timestamp) - m_originUs - delayUs;
            // Keep a monotonic sample clock. Missing/backwards timestamps must
            // never discard valid PCM or replay already output audio.
            if (m_trimmingSeek || actualUs > startUs + 2000) {
                startUs = actualUs;
            } else if (actualUs < startUs - 2000) {
                ++m_timestampRepairs;
            }
        }
        m_pcm.resize(qMax(1, capacity) * m_outputFormat.bytesPerFrame());
        uint8_t *output = reinterpret_cast<uint8_t *>(m_pcm.data());
        const int samples = m_api.swr_convert(
            m_resampler, &output, capacity,
            const_cast<const uint8_t **>(m_frame->extended_data), m_frame->nb_samples);
        m_api.av_frame_unref(m_frame);
        m_framePending = false;
        if (samples < 0) {
            return fatal(samples, QStringLiteral("转换音频格式"));
        }
        m_consecutiveErrors = 0;
        m_packetsWithoutFrame = 0;
        m_decodedAnyFrame = true;
        setPcm(samples, startUs);
        return true;
    }

    int flushResampler()
    {
        if (m_resampler == nullptr) {
            return 0;
        }
        const int available = m_api.swr_get_out_samples(m_resampler, 0);
        if (available < 0) {
            fatal(available, QStringLiteral("读取音频格式转换尾部"));
            return -1;
        }
        const int capacity = qMax(256, available);
        if (capacity > MaximumFrameBytes / m_outputFormat.bytesPerFrame()) {
            m_error = QStringLiteral("音频格式转换尾部超过允许的内存上限。");
            return -1;
        }
        m_pcm.resize(capacity * m_outputFormat.bytesPerFrame());
        uint8_t *output = reinterpret_cast<uint8_t *>(m_pcm.data());
        const int samples = m_api.swr_convert(m_resampler, &output, capacity, nullptr, 0);
        if (samples < 0) {
            fatal(samples, QStringLiteral("输出音频格式转换尾部"));
            return -1;
        }
        setPcm(samples, nextSourceUs());
        return samples;
    }

    void setPcm(int samples, qint64 startUs)
    {
        m_pcm.resize(samples * m_outputFormat.bytesPerFrame());
        m_pcmOffset = 0;
        m_pcmStartUs = startUs;
        if (startUs != nextSourceUs()) {
            m_clockAnchorUs = startUs;
            m_clockFrames = 0;
        }
        m_clockFrames += samples;
        if (m_trimmingSeek) {
            const qint64 trimUs = qMax<qint64>(0, m_seekUs - startUs);
            const qint64 trimFrames = qMin<qint64>(samples, m_api.av_rescale_rnd(
                trimUs, m_outputFormat.sampleRate(), MicrosecondsPerSecond, AV_ROUND_UP));
            m_pcmOffset = trimFrames * m_outputFormat.bytesPerFrame();
            if (trimFrames < samples) {
                m_trimmingSeek = false;
            }
        }
        if (m_fadeAfterError && m_pcmOffset < m_pcm.size() &&
            m_outputFormat.sampleFormat() == QAudioFormat::Float) {
            // A short fade-in reduces a discontinuity after skipped damage.
            // It does not claim to reconstruct the missing original waveform.
            const int channels = m_outputFormat.channelCount();
            const int frames = qMin<int>(
                (m_pcm.size() - m_pcmOffset) / m_outputFormat.bytesPerFrame(),
                qMax(1, m_outputFormat.sampleRate() / 200));
            auto *audio = reinterpret_cast<float *>(m_pcm.data() + m_pcmOffset);
            for (int frame = 0; frame < frames; ++frame) {
                const float gain = float(frame + 1) / frames;
                for (int channel = 0; channel < channels; ++channel) {
                    audio[frame * channels + channel] *= gain;
                }
            }
            m_fadeAfterError = false;
        }
    }

    FFmpegApi &m_api;
    std::shared_ptr<AudioDecoderControl> m_control;
    quint64 m_serial;
    QString m_path;
    AVFormatContext *m_format = nullptr;
    AVCodecContext *m_codec = nullptr;
    AVStream *m_audioStream = nullptr;
    AVPacket *m_packet = nullptr;
    AVFrame *m_frame = nullptr;
    SwrContext *m_resampler = nullptr;
    AVChannelLayout m_inputLayout{};
    AVChannelLayout m_outputLayout{};
    AVSampleFormat m_inputFormat = AV_SAMPLE_FMT_NONE;
    int m_inputRate = 0;
    int m_streamIndex = -1;
    QAudioFormat m_outputFormat;
    QMediaMetaData m_metadata;
    qint64 m_duration = 0;
    qint64 m_originUs = 0;
    qint64 m_seekUs = 0;
    qint64 m_clockAnchorUs = 0;
    qint64 m_clockFrames = 0;
    qint64 m_ioDeadline = 0;
    QByteArray m_pcm;
    qsizetype m_pcmOffset = 0;
    qint64 m_pcmStartUs = 0;
    int m_consecutiveErrors = 0;
    int m_packetsWithoutFrame = 0;
    qint64 m_warningCount = 0;
    qint64 m_timestampRepairs = 0;
    bool m_packetPending = false;
    bool m_framePending = false;
    bool m_demuxEnded = false;
    bool m_drainSent = false;
    bool m_trimmingSeek = true;
    bool m_decodedAnyFrame = false;
    bool m_fadeAfterError = false;
    bool m_seekable = false;
    QString m_error;
    QMediaPlayer::Error m_errorCode = QMediaPlayer::FormatError;
};

AudioDecoderWorker::AudioDecoderWorker(std::shared_ptr<AudioDecoderControl> control)
    : m_control(std::move(control)), m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &AudioDecoderWorker::pump);
}

AudioDecoderWorker::~AudioDecoderWorker()
{
    close();
}

void AudioDecoderWorker::open(quint64 serial, const QString &path, qint64 position,
                              const QAudioDevice &device)
{
    close();
    if (serial != m_control->serial.load()) {
        return;
    }
    m_serial = serial;
    m_sentFrames = 0;
    m_session = std::make_unique<DecoderSession>(m_control, serial, path, position);
    const bool opened = m_session->open(device);
    if (serial != m_control->serial.load()) {
        close();
        return;
    }
    if (!opened) {
        const QString reason = m_session->errorString();
        const auto error = m_session->errorCode();
        close();
        emit failed(serial, error, reason);
        return;
    }
    emit loaded(serial, m_session->duration(), m_session->seekable(), m_session->metadata());
    resume();
}

void AudioDecoderWorker::close()
{
    m_timer->stop();
    m_session.reset();
}

void AudioDecoderWorker::resume()
{
    if (m_session != nullptr && m_serial == m_control->serial.load() &&
        m_control->playing.load()) {
        m_timer->start(0);
    }
}

void AudioDecoderWorker::pump()
{
    if (m_session == nullptr || m_serial != m_control->serial.load() ||
        !m_control->playing.load()) {
        return;
    }
    const qint64 maximumFrames =
        qint64(m_session->outputRate()) * DecodeAheadUs / MicrosecondsPerSecond;
    if (m_sentFrames - m_control->consumedFrames.load() >= maximumFrames) {
        m_timer->start(20);
        return;
    }
    QAudioBuffer buffer;
    const DecoderSession::Result result = m_session->next(buffer);
    if (m_serial != m_control->serial.load()) {
        close();
        return;
    }
    if (result == DecoderSession::Result::Buffer) {
        m_sentFrames += buffer.frameCount();
        emit bufferReady(m_serial, buffer);
    } else if (result == DecoderSession::Result::End) {
        const quint64 serial = m_serial;
        close();
        emit bufferReady(serial, QAudioBuffer());
        emit finished(serial);
        return;
    } else if (result == DecoderSession::Result::Failed) {
        const quint64 serial = m_serial;
        const QString reason = m_session->errorString();
        const auto error = m_session->errorCode();
        close();
        emit failed(serial, error, reason);
        return;
    }
    if (m_control->playing.load()) {
        m_timer->start(0);
    }
}
