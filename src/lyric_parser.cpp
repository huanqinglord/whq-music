#include "lyric_parser.h"

#include <QRegularExpression>
#include <QStringConverter>
#include <QStringList>

#include <algorithm>
#include <array>
#include <utility>

namespace Lyrics {
namespace {
constexpr qint64 MaximumTimeMs = 24 * 60 * 60 * 1000;
constexpr int MaximumContentBytes = 2 * 1024 * 1024;
constexpr int MaximumKrcBytes = 256 * 1024;

bool readTime(const QString &text, qint64 &value)
{
    bool valid = false;
    value = text.toLongLong(&valid);
    return valid && value >= 0 && value <= MaximumTimeMs;
}

QList<Line> parseKrc(const QStringList &sourceLines)
{
    static const QRegularExpression linePattern(QStringLiteral("^\\[(\\d+),(\\d+)\\](.*)$"));
    static const QRegularExpression wordPattern(
        QStringLiteral("<(\\d+),(\\d+),\\d+>([^<]*)"));
    QList<Line> result;
    for (const QString &source : sourceLines) {
        const auto match = linePattern.match(source);
        if (!match.hasMatch()) {
            continue;
        }
        Line line;
        qint64 duration = 0;
        if (!readTime(match.captured(1), line.startMs) ||
            !readTime(match.captured(2), duration) || duration > MaximumTimeMs - line.startMs) {
            return {};
        }
        line.endMs = line.startMs + duration;
        const QString body = match.captured(3);
        auto words = wordPattern.globalMatch(body);
        qsizetype consumed = 0;
        qint64 previousStart = line.startMs;
        while (words.hasNext()) {
            const auto wordMatch = words.next();
            qint64 offset = 0;
            qint64 wordDuration = 0;
            if (wordMatch.capturedStart() != consumed ||
                !readTime(wordMatch.captured(1), offset) ||
                !readTime(wordMatch.captured(2), wordDuration) ||
                offset > MaximumTimeMs - line.startMs ||
                wordDuration > MaximumTimeMs - line.startMs - offset) {
                return {};
            }
            Word word;
            word.startMs = line.startMs + offset;
            word.endMs = word.startMs + wordDuration;
            if (word.startMs < previousStart) {
                return {};
            }
            previousStart = word.startMs;
            const QString text = wordMatch.captured(3);
            word.textStart = static_cast<int>(line.text.size());
            word.textLength = static_cast<int>(text.size());
            line.text += text;
            if (!text.isEmpty()) {
                line.words.append(word);
            }
            // 部分文件的行时长存在舍入误差，保留字词时间，不截断最后一个拖音。
            line.endMs = qMax(line.endMs, word.endMs);
            consumed = wordMatch.capturedEnd();
        }
        if (consumed != body.size()) {
            return {};
        }
        result.append(std::move(line));
    }
    return result;
}
} // namespace

QList<Line> parseTimed(const QString &content)
{
    if (content.isEmpty() || content.size() > MaximumContentBytes) {
        return {};
    }
    const QStringList sourceLines = content.split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    static const QRegularExpression krcMarker(QStringLiteral("^\\[\\d+,\\d+\\]"),
                                              QRegularExpression::MultilineOption);
    QList<Line> result;
    if (krcMarker.match(content).hasMatch()) {
        result = parseKrc(sourceLines);
    } else {
        static const QRegularExpression timestamp(
            QStringLiteral("\\[(\\d{1,3}):(\\d{2})(?:[.:](\\d{1,3}))?\\]"));
        for (const QString &source : sourceLines) {
            QString text = source;
            text.remove(timestamp);
            text = text.trimmed();
            auto matches = timestamp.globalMatch(source);
            while (matches.hasNext()) {
                const auto match = matches.next();
                const qint64 seconds = match.captured(2).toLongLong();
                if (seconds >= 60) {
                    continue;
                }
                const QString fraction = match.captured(3);
                const qint64 milliseconds = fraction.isEmpty()
                                                 ? 0
                                                 : fraction.leftJustified(3, QLatin1Char('0'))
                                                       .toLongLong();
                Line line;
                line.startMs = (match.captured(1).toLongLong() * 60 + seconds) * 1000 + milliseconds;
                line.text = text;
                result.append(std::move(line));
            }
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const Line &left, const Line &right) {
        return left.startMs < right.startMs;
    });
    return result;
}

Mode mode(const QList<Line> &lines)
{
    bool hasText = false;
    for (const Line &line : lines) {
        if (line.text.trimmed().isEmpty()) {
            continue;
        }
        hasText = true;
        if (!line.words.isEmpty()) {
            return Mode::Word;
        }
    }
    return hasText ? Mode::Line : Mode::None;
}

QString modeText(Mode mode)
{
    switch (mode) {
    case Mode::Word:
        return QStringLiteral("逐字");
    case Mode::Line:
        return QStringLiteral("逐句");
    case Mode::None:
        return QStringLiteral("无");
    }
    return QStringLiteral("无");
}

QString previewText(const QString &content)
{
    const QList<Line> lines = parseTimed(content);
    if (lines.isEmpty()) {
        return content;
    }
    QStringList text;
    for (const Line &line : lines) {
        text.append(line.text);
    }
    return text.join(QLatin1Char('\n'));
}

bool decodeKrc(const QByteArray &payload, QString &content, QString &error)
{
    content.clear();
    error.clear();
    if (payload.size() <= 4 || payload.size() > MaximumKrcBytes || !payload.startsWith("krc1")) {
        error = QStringLiteral("逐字歌词文件格式异常或文件过大");
        return false;
    }
    constexpr std::array<unsigned char, 16> key = {
        64, 71, 97, 119, 94, 50, 116, 71, 81, 54, 49, 45, 206, 210, 110, 105};
    // KRC 为文件头、循环异或后的 zlib 数据；qUncompress 还需要四字节长度提示。
    QByteArray compressed(4, '\0');
    compressed[2] = 0x40;
    for (qsizetype index = 4; index < payload.size(); ++index) {
        compressed.append(static_cast<char>(static_cast<unsigned char>(payload.at(index)) ^
                                             key[(index - 4) % key.size()]));
    }
    const QByteArray decoded = qUncompress(compressed);
    if (decoded.isEmpty() || decoded.size() > MaximumContentBytes) {
        error = QStringLiteral("逐字歌词解压失败或内容过大");
        return false;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    content = decoder(decoded);
    if (decoder.hasError() || mode(parseTimed(content)) != Mode::Word) {
        content.clear();
        error = QStringLiteral("逐字歌词编码或时间数据无效");
        return false;
    }
    return true;
}

} // namespace Lyrics
