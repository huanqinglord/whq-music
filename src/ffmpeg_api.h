#ifndef FFMPEG_API_H
#define FFMPEG_API_H

#include <QLibrary>
#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

// Use the public FFmpeg headers and check DLL ABI versions before opening a file.
// Dynamic loading lets the player share Qt's FFmpeg installation without a second codec build.
class FFmpegApi final
{
public:
    static FFmpegApi &instance();
    bool available() const;
    QString errorString() const;
    QString describeError(int code) const;

#define WHQ_FFMPEG_FUNCTION(name) decltype(&::name) name = nullptr;
    WHQ_FFMPEG_FUNCTION(avutil_version)
    WHQ_FFMPEG_FUNCTION(avcodec_version)
    WHQ_FFMPEG_FUNCTION(avformat_version)
    WHQ_FFMPEG_FUNCTION(swresample_version)
    WHQ_FFMPEG_FUNCTION(av_strerror)
    WHQ_FFMPEG_FUNCTION(av_rescale_q)
    WHQ_FFMPEG_FUNCTION(av_rescale_rnd)
    WHQ_FFMPEG_FUNCTION(av_dict_get)
    WHQ_FFMPEG_FUNCTION(av_channel_layout_default)
    WHQ_FFMPEG_FUNCTION(av_channel_layout_copy)
    WHQ_FFMPEG_FUNCTION(av_channel_layout_compare)
    WHQ_FFMPEG_FUNCTION(av_channel_layout_uninit)
    WHQ_FFMPEG_FUNCTION(avformat_alloc_context)
    WHQ_FFMPEG_FUNCTION(avformat_open_input)
    WHQ_FFMPEG_FUNCTION(avformat_find_stream_info)
    WHQ_FFMPEG_FUNCTION(av_find_best_stream)
    WHQ_FFMPEG_FUNCTION(avformat_close_input)
    WHQ_FFMPEG_FUNCTION(av_read_frame)
    WHQ_FFMPEG_FUNCTION(av_seek_frame)
    WHQ_FFMPEG_FUNCTION(avcodec_alloc_context3)
    WHQ_FFMPEG_FUNCTION(avcodec_parameters_to_context)
    WHQ_FFMPEG_FUNCTION(avcodec_open2)
    WHQ_FFMPEG_FUNCTION(avcodec_free_context)
    WHQ_FFMPEG_FUNCTION(avcodec_send_packet)
    WHQ_FFMPEG_FUNCTION(avcodec_receive_frame)
    WHQ_FFMPEG_FUNCTION(avcodec_flush_buffers)
    WHQ_FFMPEG_FUNCTION(av_packet_alloc)
    WHQ_FFMPEG_FUNCTION(av_packet_free)
    WHQ_FFMPEG_FUNCTION(av_packet_unref)
    WHQ_FFMPEG_FUNCTION(av_frame_alloc)
    WHQ_FFMPEG_FUNCTION(av_frame_free)
    WHQ_FFMPEG_FUNCTION(av_frame_unref)
    WHQ_FFMPEG_FUNCTION(swr_alloc_set_opts2)
    WHQ_FFMPEG_FUNCTION(swr_init)
    WHQ_FFMPEG_FUNCTION(swr_free)
    WHQ_FFMPEG_FUNCTION(swr_convert)
    WHQ_FFMPEG_FUNCTION(swr_get_delay)
    WHQ_FFMPEG_FUNCTION(swr_get_out_samples)
#undef WHQ_FFMPEG_FUNCTION

private:
    FFmpegApi();
    bool loadLibrary(QLibrary &library, const QString &name, int major);

    template<typename Function>
    bool resolve(QLibrary &library, Function &function, const char *name)
    {
        function = reinterpret_cast<Function>(library.resolve(name));
        if (function == nullptr) {
            m_error = QStringLiteral("FFmpeg 缺少接口 %1：%2")
                          .arg(QString::fromLatin1(name), library.errorString());
            return false;
        }
        return true;
    }

    QLibrary m_util;
    QLibrary m_codec;
    QLibrary m_format;
    QLibrary m_resample;
    QString m_error;
    bool m_available = false;
};

#endif
