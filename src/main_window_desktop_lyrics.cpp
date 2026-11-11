#include "main_window.h"
#include "desktop_lyrics_window.h"
#include "player_widgets.h"

#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QPushButton>
#include <QTimer>
#include <QVariant>

#include <algorithm>

void MainWindow::initializeDesktopLyrics()
{
    // This window has no native owner, so minimizing the player does not hide its lyrics.
    m_desktopLyrics = new DesktopLyricsWindow;
    m_desktopLyrics->restoreConfiguration(setting(QStringLiteral("desktopLyricsWindow")),
                                          frameGeometry().center());
    m_desktopLyrics->setLyricsFont(m_lyrics->font());
    connect(m_desktopLyrics, &DesktopLyricsWindow::previousRequested, this,
            &MainWindow::playPrevious);
    connect(m_desktopLyrics, &DesktopLyricsWindow::playPauseRequested, this,
            &MainWindow::togglePlayback);
    connect(m_desktopLyrics, &DesktopLyricsWindow::nextRequested, this, [this] {
        playNext();
    });
    connect(m_desktopLyrics, &DesktopLyricsWindow::playModeRequested, this,
            &MainWindow::cyclePlayMode);
    connect(m_desktopLyrics, &DesktopLyricsWindow::configurationChanged, this,
            [this](const QString &configuration) {
                saveSetting(QStringLiteral("desktopLyricsWindow"), configuration, false);
            });
    m_desktopLyricsTimer = new QTimer(this);
    m_desktopLyricsTimer->setInterval(33);
    m_desktopLyricsTimer->setTimerType(Qt::PreciseTimer);
    connect(m_desktopLyricsTimer, &QTimer::timeout, this, &MainWindow::updateDesktopLyrics);
    connect(m_desktopLyrics, &DesktopLyricsWindow::visibilityChanged, this, [this](bool visible) {
        m_desktopLyricsButton->setChecked(visible);
        if (!visible) {
            m_desktopLyricsTimer->stop();
        } else {
            updateDesktopLyrics();
        }
        updateTaskbarToolbar();
    });
    updateDesktopLyrics();
    updateTaskbarToolbar();
}

void MainWindow::toggleDesktopLyrics()
{
    if (m_desktopLyrics == nullptr) {
        return;
    }
    if (m_desktopLyrics->isVisible()) {
        m_desktopLyrics->close();
    } else {
        updateDesktopLyrics();
        m_desktopLyrics->showLyrics();
    }
}

void MainWindow::updateDesktopLyrics()
{
    if (m_desktopLyrics == nullptr) {
        return;
    }
    const auto *delegate = static_cast<PlayerWidgets::LyricsItemDelegate *>(m_lyrics->itemDelegate());
    m_desktopLyrics->setPlaybackControls(m_playButton->icon(), m_playButton->toolTip(),
                                         m_modeButton->icon(), m_modeButton->toolTip(),
                                         isEnabled() && !m_fileEditInProgress && !m_exitRequested);
    const bool animate = m_desktopLyrics->isVisible() &&
                         m_player->playbackState() == QMediaPlayer::PlayingState;
    if (animate && !m_desktopLyricsTimer->isActive()) {
        m_desktopLyricsTimer->start();
    } else if (!animate) {
        m_desktopLyricsTimer->stop();
    }
    if (m_lyricLines.isEmpty()) {
        Lyrics::Line title;
        title.text = m_detailTitle->text();
        const bool plainLyrics =
            m_lyrics->count() > 0 && m_missingLyricsAudioPath.isEmpty() &&
            !m_lyrics->item(0)->data(Qt::UserRole).isValid() &&
            m_lyrics->item(0)->text() != QStringLiteral("暂无歌词");
        m_desktopLyrics->setLines(title, plainLyrics ? QStringLiteral("纯文本歌词")
                                                     : QStringLiteral("暂无歌词"),
                                  0.0);
        return;
    }
    const qint64 position = m_player->position();
    const auto next = std::upper_bound(
        m_lyricLines.cbegin(), m_lyricLines.cend(), position,
        [](qint64 time, const Lyrics::Line &line) {
            return time < line.startMs;
        });
    const int index = qMax(0, static_cast<int>(next - m_lyricLines.cbegin()) - 1);
    const Lyrics::Line &current = m_lyricLines.at(index);
    const QString upcoming = index + 1 < m_lyricLines.size()
                                 ? m_lyricLines.at(index + 1).text
                                 : QString();
    const qint64 nextStart = index + 1 < m_lyricLines.size()
                                ? m_lyricLines.at(index + 1).startMs
                                : m_player->duration();
    const qint64 end = current.endMs > current.startMs ? current.endMs : nextStart;
    const qint64 duration = end > current.startMs ? end - current.startMs : 6000;
    qreal progress = 0.0;
    if (!current.words.isEmpty() && !current.text.isEmpty()) {
        progress = delegate->wordFillProgress(current, position);
    } else if (position >= current.startMs) {
        progress = qreal(position - current.startMs) / duration;
    }
    const qint64 hold = qMin(qint64(600), duration / 5);
    const qreal scrollProgress = !current.words.isEmpty()
                                     ? progress
                                     : qreal(position - current.startMs - hold) /
                                           qMax(qint64(1), duration - 2 * hold);
    // Keep each sentence in its previewed row or column, including after seeking.
    m_desktopLyrics->setLines(current, upcoming, progress, scrollProgress, index % 2);
}
