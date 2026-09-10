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
    if (content.trimmed().isEmpty()) {
        return false;
    }
    const QRegularExpression timestamp(
        QStringLiteral("\\[(\\d{1,2}):(\\d{2})(?:[.:](\\d{1,3}))?\\]"));
    const QStringList lines =
        content.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        auto iterator = timestamp.globalMatch(line);
        QString text = line;
        text.remove(timestamp);
        text = text.trimmed();
        while (iterator.hasNext()) {
            const auto match = iterator.next();
            const qint64 minutes = match.captured(1).toLongLong();
            const qint64 seconds = match.captured(2).toLongLong();
            QString fraction = match.captured(3);
            const qint64 milliseconds =
                fraction.isEmpty()
                    ? 0
                    : fraction.leftJustified(3, QLatin1Char('0')).left(3).toLongLong();
            m_lyricLines.append({(minutes * 60 + seconds) * 1000 + milliseconds, text});
        }
    }
    std::sort(m_lyricLines.begin(), m_lyricLines.end(), [](const auto &left, const auto &right) {
        return left.first < right.first;
    });
    for (const auto &line : std::as_const(m_lyricLines)) {
        const QString lyricText =
            line.second.isEmpty() ? QStringLiteral("···") : displayedLyricsText(line.second);
        auto *item = new QListWidgetItem(lyricText);
        item->setTextAlignment(Qt::AlignCenter);
        item->setData(Qt::UserRole, line.first);
        m_lyrics->addItem(item);
    }
    if (m_lyricLines.isEmpty()) {
        return false;
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
        [](qint64 time, const QPair<qint64, QString> &line) {
            return time < line.first;
        });
    const int activeIndex = static_cast<int>(next - m_lyricLines.cbegin()) - 1;
    const bool playing = m_player->playbackState() == QMediaPlayer::PlayingState;
    qint64 nextUpdate = next != m_lyricLines.cend() ? next->first - position : -1;
    auto *delegate = static_cast<LyricsItemDelegate *>(m_lyrics->itemDelegate());
    if (activeIndex < 0) {
        if (m_currentLyricIndex >= 0 && m_currentLyricIndex < m_lyrics->count()) {
            m_lyrics->item(m_currentLyricIndex)->setData(Qt::UserRole + 1, false);
        }
        m_currentLyricIndex = -1;
        delegate->setScrollProgress(0.0);
        if (playing && nextUpdate > 0) {
            m_lyricsScrollTimer->start(static_cast<int>(qBound(qint64(16), nextUpdate,
                                                              qint64(60000))));
        }
        m_lyricsViewDirty = false;
        return;
    }

    const qint64 start = m_lyricLines.at(activeIndex).first;
    const qint64 end = activeIndex + 1 < m_lyricLines.size()
                           ? m_lyricLines.at(activeIndex + 1).first
                           : m_player->duration();
    const qint64 lineDuration = end > start ? end - start : 6000;
    // 根据歌曲时间计算位移：首尾各停留一小段，暂停和拖动进度时保持同步。
    const qint64 holdDuration = qMin(qint64(600), lineDuration / 5);
    const qint64 scrollDuration = qMax(qint64(1), lineDuration - 2 * holdDuration);
    if (activeIndex >= m_lyrics->count()) {
        return;
    }
    const bool longLine = delegate->needsHorizontalScroll(
        m_lyrics->model()->index(activeIndex, 0), m_lyrics->viewport()->width());
    const bool scrollChanged = delegate->setScrollProgress(
        longLine ? static_cast<qreal>(position - start - holdDuration) / scrollDuration : 0.0);
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
    if (lineChanged || m_lyricsViewDirty || (longLine && scrollChanged)) {
        m_lyrics->viewport()->update(m_lyrics->visualItemRect(m_lyrics->item(activeIndex)));
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
    // Short lines and the pauses at either end of a long line need only a boundary wakeup.
    if (playing && nextUpdate > 0) {
        m_lyricsScrollTimer->start(
            static_cast<int>(qBound(qint64(16), nextUpdate, qint64(60000))));
    }
}
