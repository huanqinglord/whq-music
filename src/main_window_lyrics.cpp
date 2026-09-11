#include "main_window.h"
#include "player_widgets.h"
#include "music_file_utils.h"
#include "audio_file_editor.h"
#include "lyrics_download_dialog.h"

#include <QAbstractItemView>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#include <shobjidl.h>
#include <windows.h>

#endif

#include <fileref.h>
#include <tag.h>
#include <tfile.h>
#include <tpropertymap.h>

using namespace PlayerWidgets;

void MainWindow::loadLyrics(const QString &audioPath)
{
    m_lyricsViewDirty = true;
    if (m_lyricsScrollTimer != nullptr) {
        m_lyricsScrollTimer->stop();
    }
    m_lyricLines.clear();
    m_currentLyricIndex = -1;
    m_missingLyricsAudioPath.clear();
    m_lyrics->clear();
    const QString synchronizedLyrics = embeddedLyrics(audioPath, true);
    if (loadTimestampedLyrics(synchronizedLyrics)) {
        return;
    }

    const QString plainLyrics = embeddedLyrics(audioPath, false);
    if (!plainLyrics.trimmed().isEmpty()) {
        // 歌词标签也可保存带时间戳的 LRC 文本，先尝试滚动解析。
        if (!loadTimestampedLyrics(plainLyrics)) {
            showPlainLyrics(plainLyrics);
        }
        return;
    }
    if (!synchronizedLyrics.trimmed().isEmpty()) {
        showPlainLyrics(synchronizedLyrics);
        return;
    }
    const QFileInfo audioInfo(audioPath);
    const QString lyricPath =
        audioInfo.dir().filePath(audioInfo.completeBaseName() + QStringLiteral(".lrc"));
    QFile lyricFile(lyricPath);
    if (lyricFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&lyricFile);
        const QString content = stream.readAll();
        if (loadTimestampedLyrics(content)) {
            return;
        }
        if (!content.trimmed().isEmpty()) {
            showPlainLyrics(content);
            return;
        }
    }
    showMissingLyrics(audioPath);
}

QString MainWindow::displayedLyricsText(const QString &text) const
{
    if (!m_showSimplifiedLyrics || text.isEmpty()) {
        return text;
    }

#ifdef Q_OS_WIN
    const int convertedLength = LCMapStringEx(LOCALE_NAME_SYSTEM_DEFAULT, LCMAP_SIMPLIFIED_CHINESE,
                                              reinterpret_cast<LPCWSTR>(text.utf16()), text.size(),
                                              nullptr, 0, nullptr, nullptr, 0);
    if (convertedLength <= 0) {
        return text;
    }
    QString converted(convertedLength, Qt::Uninitialized);
    const int writtenLength = LCMapStringEx(LOCALE_NAME_SYSTEM_DEFAULT, LCMAP_SIMPLIFIED_CHINESE,
                                            reinterpret_cast<LPCWSTR>(text.utf16()), text.size(),
                                            reinterpret_cast<LPWSTR>(converted.data()),
                                            convertedLength, nullptr, nullptr, 0);
    if (writtenLength <= 0) {
        return text;
    }
    converted.resize(writtenLength);
    return converted;
#else
    return text;
#endif
}

void MainWindow::updateLyricsScriptButton()
{
    if (m_lyricsScriptButton == nullptr) {
        return;
    }
    m_lyricsScriptButton->setText(m_showSimplifiedLyrics ? QStringLiteral("简")
                                                         : QStringLiteral("繁"));
    m_lyricsScriptButton->setToolTip(m_showSimplifiedLyrics ? QStringLiteral("切换为原始繁体歌词")
                                                            : QStringLiteral("切换为简体歌词"));
    m_lyricsScriptButton->setAccessibleName(m_lyricsScriptButton->toolTip());
}

void MainWindow::showMissingLyrics(const QString &audioPath)
{
    m_missingLyricsAudioPath = audioPath;
    auto *item = new QListWidgetItem(QStringLiteral("暂无歌词，点击下载歌词"));
    item->setTextAlignment(Qt::AlignCenter);
    item->setData(Qt::UserRole + 2, true);
    m_lyrics->addItem(item);
}

void MainWindow::requestLyricsDownload(const QString &audioPath)
{
    if (audioPath.isEmpty()) {
        return;
    }
    if (!QFileInfo(audioPath).isFile()) {
        QMessageBox::warning(this, QStringLiteral("无法下载歌词"),
                             QStringLiteral("本地音乐文件不存在，请刷新音乐库后重试。"));
        return;
    }

    auto *surface = static_cast<AppSurface *>(centralWidget());
    LyricsDownloadDialog dialog(audioPath, m_networkManager, surface,
                                surface->blurredRegion(surface), this);
    const QPointer<LyricsDownloadDialog> target(&dialog);
    connect(&dialog, &LyricsDownloadDialog::embeddedSaveRequested, this,
            [this, target](const QString &path, const QString &lyrics, bool overwriteConfirmed) {
                AudioEditRequest request;
                request.originalPath = path;
                request.targetPath = path;
                request.databasePath = m_database.databaseName();
                request.lyricsOnly = true;
                request.lyrics = lyrics;
                request.overwriteConfirmed = overwriteConfirmed;
                startAudioFileEdit(request, [target](const AudioEditResult &result,
                                                     bool needsOverwrite) {
                    if (target != nullptr) {
                        target->finishEmbeddedSave(result.success, needsOverwrite, result.message);
                    }
                });
            });
    connect(&dialog, &LyricsDownloadDialog::lyricsSaved, this, [this](const QString &audioPath) {
        // 非播放歌曲也可下载；只更新仍在播放该目标的歌词界面。
        if (m_player != nullptr && m_player->source().isLocalFile() &&
            normalizedPath(m_player->source().toLocalFile()) == normalizedPath(audioPath)) {
            loadLyrics(audioPath);
            updateLyrics(m_player->position());
        }
    });
    dialog.exec();
}

bool MainWindow::loadTimestampedLyrics(const QString &content)
{
    m_lyricLines = Lyrics::parseTimed(content);
    if (m_lyricLines.isEmpty()) {
        return false;
    }
    for (Lyrics::Line &line : m_lyricLines) {
        if (line.words.isEmpty()) {
            line.text = displayedLyricsText(line.text);
        } else {
            // 按字词转换繁简体并同步文本位置，不能转换整句后继续沿用旧字符下标。
            const QString original = line.text;
            QString displayed;
            int consumed = 0;
            for (Lyrics::Word &word : line.words) {
                const int originalStart = word.textStart;
                const int originalLength = word.textLength;
                displayed += displayedLyricsText(original.mid(consumed, originalStart - consumed));
                word.textStart = static_cast<int>(displayed.size());
                const QString text = displayedLyricsText(original.mid(originalStart, originalLength));
                displayed += text;
                word.textLength = static_cast<int>(text.size());
                consumed = originalStart + originalLength;
            }
            displayed += displayedLyricsText(original.mid(consumed));
            line.text = displayed;
        }
        const QString lyricText = line.text.isEmpty() ? QStringLiteral("···") : line.text;
        auto *item = new QListWidgetItem(lyricText);
        item->setTextAlignment(Qt::AlignCenter);
        item->setData(Qt::UserRole, line.startMs);
        m_lyrics->addItem(item);
    }
    auto *delegate = static_cast<LyricsItemDelegate *>(m_lyrics->itemDelegate());
    const auto next = std::upper_bound(
        m_lyricLines.cbegin(), m_lyricLines.cend(), m_player->position(),
        [](qint64 position, const Lyrics::Line &line) {
            return position < line.startMs;
        });
    const int initialRow = qMax(0, static_cast<int>(next - m_lyricLines.cbegin()) - 1);
    for (int row = initialRow; row < m_lyricLines.size() && row <= initialRow + 1; ++row) {
        delegate->prepareLine(m_lyricLines.at(row));
    }
    if (m_lyricsScrollTimer != nullptr) {
        // 等列表完成初始布局，再按当前播放位置定位，前奏期间也显示居中的首句。
        m_lyricsScrollTimer->start(0);
    }
    return true;
}

QString MainWindow::embeddedLyrics(const QString &audioPath, bool synchronized) const
{
#ifdef Q_OS_WIN
    TagLib::FileRef file(audioPath.toStdWString().c_str(), false);
#else
    TagLib::FileRef file(QFile::encodeName(audioPath).constData(), false);
#endif
    if (file.isNull() || file.file() == nullptr) {
        return {};
    }

    const TagLib::PropertyMap properties = file.file()->properties();
    const QList<QByteArray> keys =
        synchronized
            ? QList<QByteArray>{QByteArrayLiteral("SYNCEDLYRICS"),
                                QByteArrayLiteral("SYNCHRONIZEDLYRICS")}
            : QList<QByteArray>{QByteArrayLiteral("LYRICS"), QByteArrayLiteral("UNSYNCEDLYRICS")};
    for (const QByteArray &key : keys) {
        const auto iterator =
            properties.find(TagLib::String(key.constData(), TagLib::String::UTF8));
        if (iterator == properties.end() || iterator->second.isEmpty()) {
            continue;
        }
        const std::string lyrics = iterator->second.front().to8Bit(true);
        const QString content = QString::fromUtf8(lyrics.c_str(), static_cast<int>(lyrics.size()));
        if (!content.trimmed().isEmpty()) {
            return content;
        }
    }
    return {};
}

void MainWindow::showPlainLyrics(const QString &content)
{
    const QStringList lines =
        content.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        auto *item = new QListWidgetItem(displayedLyricsText(line.trimmed()));
        item->setTextAlignment(Qt::AlignCenter);
        m_lyrics->addItem(item);
    }
}

void MainWindow::updateLyrics(qint64 position)
{
    if (m_lyricsScrollTimer == nullptr) {
        return;
    }
    m_lyricsScrollTimer->stop();
    if (!isVisible() || isMinimized() || !m_lyrics->isVisible()) {
        m_lyricsViewDirty = true;
        return;
    }
    const auto next = std::upper_bound(
        m_lyricLines.cbegin(), m_lyricLines.cend(), position,
        [](qint64 time, const Lyrics::Line &line) {
            return time < line.startMs;
        });
    const int activeIndex = static_cast<int>(next - m_lyricLines.cbegin()) - 1;
    const bool playing = m_player->playbackState() == QMediaPlayer::PlayingState;
    qint64 nextUpdate = next != m_lyricLines.cend() ? next->startMs - position : -1;
    auto *delegate = static_cast<LyricsItemDelegate *>(m_lyrics->itemDelegate());
    if (activeIndex < 0) {
        const bool needsPositioning = m_lyricsViewDirty || m_currentLyricIndex >= 0;
        if (m_currentLyricIndex >= 0 && m_currentLyricIndex < m_lyrics->count()) {
            m_lyrics->item(m_currentLyricIndex)->setData(Qt::UserRole + 1, false);
        }
        m_currentLyricIndex = -1;
        delegate->setScrollProgress(0.0);
        delegate->setFillProgress(0.0);
        if (needsPositioning && !m_lyricLines.isEmpty() && m_lyrics->count() > 0) {
            m_lyrics->scrollToItem(m_lyrics->item(0), QAbstractItemView::PositionAtCenter);
            m_lyrics->viewport()->update();
        }
        if (playing && nextUpdate > 0) {
            m_lyricsScrollTimer->start(static_cast<int>(qBound(qint64(16), nextUpdate,
                                                              qint64(60000))));
        }
        m_lyricsViewDirty = false;
        return;
    }

    const Lyrics::Line &activeLine = m_lyricLines.at(activeIndex);
    const qint64 start = activeLine.startMs;
    const qint64 nextStart = activeIndex + 1 < m_lyricLines.size()
                                ? m_lyricLines.at(activeIndex + 1).startMs
                                : m_player->duration();
    const qint64 end = activeLine.endMs > start ? activeLine.endMs : nextStart;
    const qint64 lineDuration = end > start ? end - start : 6000;
    // 根据歌曲时间计算位移：首尾各停留一小段，暂停和拖动进度时保持同步。
    const qint64 holdDuration = qMin(qint64(600), lineDuration / 5);
    const qint64 scrollDuration = qMax(qint64(1), lineDuration - 2 * holdDuration);
    if (activeIndex >= m_lyrics->count()) {
        return;
    }
    const bool lineChanged = activeIndex != m_currentLyricIndex;
    if (lineChanged) {
        if (m_currentLyricIndex >= 0 && m_currentLyricIndex < m_lyrics->count()) {
            m_lyrics->item(m_currentLyricIndex)->setData(Qt::UserRole + 1, false);
        }
        m_currentLyricIndex = activeIndex;
        m_lyrics->item(activeIndex)->setData(Qt::UserRole + 1, true);
    }
    if (lineChanged || m_lyricsViewDirty) {
        m_lyrics->scrollToItem(m_lyrics->item(activeIndex), QAbstractItemView::PositionAtCenter);
    }
    if (lineChanged || m_lyricsViewDirty) {
        delegate->prepareLine(activeLine);
        // 下一句在当前句已开始显示后预热，不把预热开销放在下一次换行的临界点。
        QTimer::singleShot(40, this, [this] {
            const int nextRow = m_currentLyricIndex + 1;
            if (nextRow >= 0 && nextRow < m_lyricLines.size()) {
                auto *currentDelegate = static_cast<LyricsItemDelegate *>(m_lyrics->itemDelegate());
                currentDelegate->prepareLine(m_lyricLines.at(nextRow));
            }
        });
    }
    // 排版和居中完成后使用最新已上报的音频位置，不沿用准备之前的时间计算首帧填色。
    position = m_player->position();
    if (position < start || (next != m_lyricLines.cend() && position >= next->startMs)) {
        m_lyricsViewDirty = true;
        m_lyricsScrollTimer->start(0);
        return;
    }
    nextUpdate = next != m_lyricLines.cend() ? next->startMs - position : -1;
    const bool longLine = delegate->needsHorizontalScroll(
        m_lyrics->model()->index(activeIndex, 0), m_lyrics->viewport()->width());
    // 有字词时间时按真实时间和排版宽度填色；普通 LRC 保持句内匀速填色。
    const bool wordTimed = !activeLine.words.isEmpty();
    const qreal fillProgress = wordTimed
                                   ? delegate->wordFillProgress(activeLine, position)
                                   : static_cast<qreal>(position - start) / lineDuration;
    const qreal scrollProgress = wordTimed
                                     ? fillProgress
                                     : static_cast<qreal>(position - start - holdDuration) / scrollDuration;
    const bool scrollChanged = delegate->setScrollProgress(longLine ? scrollProgress : 0.0);
    const bool fillChanged = delegate->setFillProgress(fillProgress);
    if (lineChanged || m_lyricsViewDirty || fillChanged || (longLine && scrollChanged)) {
        // 列表项可能只有占位宽度，实际歌词铺满视口，动画必须重绘整行。
        QRect repaintArea = m_lyrics->visualItemRect(m_lyrics->item(activeIndex));
        repaintArea.setLeft(0);
        repaintArea.setRight(m_lyrics->viewport()->width() - 1);
        m_lyrics->viewport()->update(repaintArea);
    }
    m_lyricsViewDirty = false;
    if (playing && longLine) {
        const qint64 scrollStart = start + holdDuration;
        const qint64 scrollEnd = scrollStart + scrollDuration;
        const qint64 animationDelay = position < scrollStart ? scrollStart - position
                                                            : (position < scrollEnd ? 33 : -1);
        if (animationDelay > 0) {
            nextUpdate = nextUpdate > 0 ? qMin(nextUpdate, animationDelay) : animationDelay;
        }
    }
    // 短句同样需要填色动画；填满后停止帧更新，只等待下一句，暂停时不启动定时器。
    if (playing && position - start < lineDuration) {
        const qint64 fillDelay = qMin(qint64(33), lineDuration - (position - start));
        nextUpdate = nextUpdate > 0 ? qMin(nextUpdate, fillDelay) : fillDelay;
    }
    if (playing && nextUpdate > 0) {
        m_lyricsScrollTimer->start(
            static_cast<int>(qBound(qint64(16), nextUpdate, qint64(60000))));
    }
}
