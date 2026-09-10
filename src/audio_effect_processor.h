#ifndef AUDIO_EFFECT_PROCESSOR_H
#define AUDIO_EFFECT_PROCESSOR_H

#include <QAudioBuffer>
#include <QAudioFormat>
#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>

#include <array>
#include <atomic>
#include <vector>

class QAudioSink;
class QIODevice;
class QTimer;

class AudioEffectProcessor final : public QObject
{
    Q_OBJECT

public:
    enum class Preset
    {
        Original = 0,
        ClearVocal = 1,
        BassBoost = 2,
        Live = 3
    };

    explicit AudioEffectProcessor(QObject *parent = nullptr);
    ~AudioEffectProcessor() override;

    // 控制命令和输入缓冲可跨线程提交；设备与滤波状态只由所属线程访问。
    void setPreset(Preset preset);
    Preset preset() const;
    quint64 generation() const;
    void setVolume(qreal volume);
    void setPaused(bool paused);
    void reset();
    void setStream(quint64 stream);
    void finishStream(quint64 request);

signals:
    void outputFailed(quint64 generation, const QString &reason);
    void outputDrained(quint64 request, quint64 generation);
    void outputProgress(quint64 stream, qint64 processedFrames);

public slots:
    void processBuffer(quint64 stream, const QAudioBuffer &buffer);

private:
    struct Biquad
    {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double z1 = 0.0;
        double z2 = 0.0;

        float process(float sample);
        void clear();
    };

    struct ChannelState
    {
        std::array<Biquad, 3> filters;
    };

    void processQueuedBuffer(const QAudioBuffer &buffer);
    bool ensureOutput(const QAudioFormat &format);
    void flushOutput();
    void checkDrainCompletion();
    void reportProgress();
    void failOutput(const QString &reason);
    void configureFilters();
    void configurePeaking(Biquad &filter, double frequency, double gainDb,
                          double quality) const;
    void configureLowShelf(Biquad &filter, double frequency, double gainDb) const;
    void configureHighPass(Biquad &filter, double frequency, double quality) const;
    float processSample(float sample, int channel);
    void processSamples(QByteArray &data);
    void stopOutput();

    // 只保护命令入队和输入队列计数，不在持锁期间执行 DSP 或设备操作。
    mutable QMutex m_dispatchMutex;
    std::atomic<quint64> m_generation{0};
    Preset m_requestedPreset = Preset::Original;
    qint64 m_queuedInputUs = 0;
    bool m_inputOverflow = false;

    quint64 m_stream = 0;
    quint64 m_outputGeneration = 0;
    bool m_failed = false;
    Preset m_preset = Preset::Original;
    QAudioFormat m_format;
    QAudioSink *m_sink = nullptr;
    QIODevice *m_outputDevice = nullptr;
    QTimer *m_flushTimer = nullptr;
    QTimer *m_progressTimer = nullptr;
    quint64 m_outputStream = 0;
    qint64 m_writtenBytes = 0;
    qint64 m_inputFrames = 0;
    qint64 m_reportedFrames = -1;
    /** 已处理但尚未完整写入设备的音频字节，短写尾部保留到下次续写。 */
    QByteArray m_pendingData;
    qsizetype m_pendingOffset = 0;
    std::vector<ChannelState> m_channels;
    std::vector<float> m_delayLine;
    qsizetype m_delayPosition = 0;
    qreal m_volume = 0.7;
    float m_preGain = 0.64F;
    int m_fadeFrames = 1;
    int m_fadeFramesRemaining = 0;
    bool m_paused = true;
    bool m_prebuffering = true;
    bool m_outputStarted = false;
    bool m_endOfStream = false;
    quint64 m_drainRequest = 0;
    bool m_tailAppended = false;
    QElapsedTimer m_drainElapsed;
    qint64 m_drainActiveMs = 0;
};

#endif // AUDIO_EFFECT_PROCESSOR_H
