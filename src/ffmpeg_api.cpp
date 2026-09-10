#include "ffmpeg_api.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>

FFmpegApi &FFmpegApi::instance()
{
    static FFmpegApi api;
    return api;
}

FFmpegApi::FFmpegApi()
{
    if (!loadLibrary(m_util, QStringLiteral("avutil"), LIBAVUTIL_VERSION_MAJOR) ||
        !loadLibrary(m_resample, QStringLiteral("swresample"), LIBSWRESAMPLE_VERSION_MAJOR) ||
        !loadLibrary(m_codec, QStringLiteral("avcodec"), LIBAVCODEC_VERSION_MAJOR) ||
        !loadLibrary(m_format, QStringLiteral("avformat"), LIBAVFORMAT_VERSION_MAJOR)) {
        return;
    }
#define WHQ_RESOLVE(library, name) \
    if (!resolve(library, name, #name)) { \
        return; \
    }
    WHQ_RESOLVE(m_util, avutil_version)
    WHQ_RESOLVE(m_codec, avcodec_version)
    WHQ_RESOLVE(m_format, avformat_version)
    WHQ_RESOLVE(m_resample, swresample_version)
    if ((avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR ||
        (avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR ||
        (avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR ||
        (swresample_version() >> 16) != LIBSWRESAMPLE_VERSION_MAJOR) {
        m_error = QStringLiteral("FFmpeg 动态库版本与播放器不兼容，请重新部署播放依赖。");
        return;
    }
    WHQ_RESOLVE(m_util, av_strerror)
    WHQ_RESOLVE(m_util, av_rescale_q)
    WHQ_RESOLVE(m_util, av_rescale_rnd)
    WHQ_RESOLVE(m_util, av_dict_get)
    WHQ_RESOLVE(m_util, av_channel_layout_default)
    WHQ_RESOLVE(m_util, av_channel_layout_copy)
    WHQ_RESOLVE(m_util, av_channel_layout_compare)
    WHQ_RESOLVE(m_util, av_channel_layout_uninit)
    WHQ_RESOLVE(m_format, avformat_alloc_context)
    WHQ_RESOLVE(m_format, avformat_open_input)
    WHQ_RESOLVE(m_format, avformat_find_stream_info)
    WHQ_RESOLVE(m_format, av_find_best_stream)
    WHQ_RESOLVE(m_format, avformat_close_input)
    WHQ_RESOLVE(m_format, av_read_frame)
    WHQ_RESOLVE(m_format, av_seek_frame)
    WHQ_RESOLVE(m_codec, avcodec_alloc_context3)
    WHQ_RESOLVE(m_codec, avcodec_parameters_to_context)
    WHQ_RESOLVE(m_codec, avcodec_open2)
    WHQ_RESOLVE(m_codec, avcodec_free_context)
    WHQ_RESOLVE(m_codec, avcodec_send_packet)
    WHQ_RESOLVE(m_codec, avcodec_receive_frame)
    WHQ_RESOLVE(m_codec, avcodec_flush_buffers)
    WHQ_RESOLVE(m_codec, av_packet_alloc)
    WHQ_RESOLVE(m_codec, av_packet_free)
    WHQ_RESOLVE(m_codec, av_packet_unref)
    WHQ_RESOLVE(m_util, av_frame_alloc)
    WHQ_RESOLVE(m_util, av_frame_free)
    WHQ_RESOLVE(m_util, av_frame_unref)
    WHQ_RESOLVE(m_resample, swr_alloc_set_opts2)
    WHQ_RESOLVE(m_resample, swr_init)
    WHQ_RESOLVE(m_resample, swr_free)
    WHQ_RESOLVE(m_resample, swr_convert)
    WHQ_RESOLVE(m_resample, swr_get_delay)
    WHQ_RESOLVE(m_resample, swr_get_out_samples)
#undef WHQ_RESOLVE
    m_available = true;
}

bool FFmpegApi::loadLibrary(QLibrary &library, const QString &name, int major)
{
#ifdef Q_OS_WIN
    const QString fileName = QStringLiteral("%1-%2.dll").arg(name).arg(major);
    const QString localPath = QDir(QCoreApplication::applicationDirPath()).filePath(fileName);
    const QString qtPath = QDir(QLibraryInfo::path(QLibraryInfo::BinariesPath)).filePath(fileName);
    library.setFileName(QFileInfo::exists(localPath) ? localPath : qtPath);
#else
    library.setFileNameAndVersion(name, major);
#endif
    library.setLoadHints(QLibrary::PreventUnloadHint | QLibrary::ResolveAllSymbolsHint);
    if (!library.load()) {
        m_error = QStringLiteral("无法加载播放依赖 %1：%2").arg(name, library.errorString());
        return false;
    }
    return true;
}

bool FFmpegApi::available() const
{
    return m_available;
}

QString FFmpegApi::errorString() const
{
    return m_error;
}

QString FFmpegApi::describeError(int code) const
{
    char message[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, message, sizeof(message));
    return QString::fromUtf8(message);
}
