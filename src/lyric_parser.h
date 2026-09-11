#ifndef LYRIC_PARSER_H
#define LYRIC_PARSER_H

#include <QByteArray>
#include <QList>
#include <QString>

namespace Lyrics {

enum class Mode
{
    None = 0,
    Line = 1,
    Word = 2
};

struct Word
{
    /** 字词开始时间，相对于歌曲开头，单位为毫秒。 */
    qint64 startMs = 0;
    /** 字词结束时间，相对于歌曲开头，单位为毫秒。 */
    qint64 endMs = 0;
    /** 在所属行文本中的 UTF-16 起始位置。 */
    int textStart = 0;
    /** 字词文本的 UTF-16 长度。 */
    int textLength = 0;
};

struct Line
{
    qint64 startMs = 0;
    /** KRC 中的行结束时间，单位为毫秒；逐句 LRC 未提供时为 -1。 */
    qint64 endMs = -1;
    QString text;
    QList<Word> words;
};

/** 解析逐句 LRC 或解码后的 KRC，结果按开始时间稳定排序。 */
QList<Line> parseTimed(const QString &content);

/** 根据实际解析到的时间数据识别模式，不能仅凭文件扩展名判定逐字。 */
Mode mode(const QList<Line> &lines);

QString modeText(Mode mode);

/** 预览时去掉时间标记，但保存仍使用包含完整时间数据的原始文本。 */
QString previewText(const QString &content);

/** 解码下载接口 Base64 解码后的 KRC 二进制，不修改原音频文件。 */
bool decodeKrc(const QByteArray &payload, QString &content, QString &error);

} // namespace Lyrics

#endif // LYRIC_PARSER_H
