#include "main_window.h"
#include "player_widgets.h"
#include "music_library_repository.h"
#include "playback_controller.h"
#include "metadata_scanner.h"

#include <QAction>
#include <QCursor>
#include <QDebug>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QListWidget>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolTip>
#include <QVariant>

#include <utility>

using namespace PlayerWidgets;

void MainWindow::playGroup(qint64 groupId)
{
    if (m_fileEditInProgress) {
        return;
    }
    // 按分组直接读取完整歌曲列表，不受音乐库搜索条件影响。
    QSqlQuery query(m_database);
    if (groupId > 0) {
        query.prepare(QStringLiteral("SELECT s.id,s.artist,s.path FROM songs s "
                                     "JOIN song_groups sg ON sg.song_id=s.id "
                                     "WHERE sg.group_id=? ORDER BY s.title,s.id"));
        query.addBindValue(groupId);
    } else {
        query.prepare(QStringLiteral("SELECT id,artist,path FROM songs ORDER BY title,id"));
    }
    if (!query.exec()) {
        QMessageBox::warning(this, QStringLiteral("无法播放"),
                             QStringLiteral("无法读取分组歌曲，请稍后重试。"));
        return;
    }
    QList<Song> songs;
    while (query.next()) {
        const QString path = query.value(2).toString();
        songs.append({query.value(0).toLongLong(), QFileInfo(path).completeBaseName(),
                      query.value(1).toString(), path});
    }
    if (songs.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("暂无歌曲"),
                                 QStringLiteral("当前分组没有歌曲，请先添加音乐。"));
        return;
    }
    m_playbackController->replaceAndPlay(songs);
    m_contentStack->setCurrentIndex(0);
}

void MainWindow::addSongToPlaybackQueue(qint64 songId)
{
    for (const Song &song : m_playbackController->queue()) {
        if (song.id == songId) {
            QToolTip::showText(QCursor::pos(), QStringLiteral("这首歌已在播放列表中"),
                               m_songsTable);
            return;
        }
    }
    // 菜单弹出期间歌曲可能被刷新或改名，按固定 ID 获取最新数据。
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT artist,path FROM songs WHERE id=?"));
    query.addBindValue(songId);
    if (!query.exec()) {
        QMessageBox::warning(this, QStringLiteral("添加失败"),
                             QStringLiteral("无法读取歌曲信息，请稍后重试。"));
        return;
    }
    if (!query.next()) {
        QMessageBox::information(this, QStringLiteral("歌曲不存在"),
                                 QStringLiteral("这首歌已从音乐库移除，请刷新后重试。"));
        return;
    }
    const QString path = query.value(1).toString();
    m_playbackController->append(
        {songId, QFileInfo(path).completeBaseName(), query.value(0).toString(), path}, false);
    updateTaskbarToolbar();
    QToolTip::showText(QCursor::pos(), QStringLiteral("已加入播放列表末尾"), m_songsTable);
}

void MainWindow::playRow(int row)
{
    if (m_fileEditInProgress || row < 0 || row >= m_songs.size()) {
        return;
    }
    m_playbackController->append(m_songs.at(row), true);
    m_contentStack->setCurrentIndex(0);
}

void MainWindow::playQueueRow(int row)
{
    if (!m_fileEditInProgress) {
        m_playbackController->playIndex(row);
        m_contentStack->setCurrentIndex(0);
    }
}

void MainWindow::showPlayingSong(const Song &song)
{
    m_playbackErrorShown = false;
    m_detailTitle->setText(song.title);
    m_artist->setText(song.artist.isEmpty() ? QStringLiteral("未知歌手") : song.artist);
    m_album->setText(QStringLiteral("未知专辑"));
    m_cover->setPixmap({});
    m_cover->setText(QStringLiteral("♫"));
    m_queue->setCurrentRow(m_playbackController->currentIndex());
    loadLyrics(song.path);
    updatePlayingIndicators();
}

void MainWindow::clearPlayingSong()
{
    m_detailTitle->setText(QStringLiteral("未播放歌曲"));
    m_artist->clear();
    m_album->clear();
    m_cover->setPixmap({});
    m_cover->setText(QStringLiteral("♫"));
    m_lyrics->clear();
    m_lyricLines.clear();
    m_currentLyricIndex = -1;
    m_missingLyricsAudioPath.clear();
    updatePlayButton();
    updatePlayingIndicators();
}

void MainWindow::reportPlaybackError(const QString &message)
{
    Q_UNUSED(message);
    m_playbackErrorShown = true;
    m_audioDrainRequest = 0;
    m_player->stop();
    m_player->setSource({});
    for (const Song &song : std::as_const(m_librarySongs)) {
        if (song.id == m_playbackController->currentId()) {
            m_detailTitle->setText(song.title);
            break;
        }
    }
    if (m_lyricsScrollTimer != nullptr) {
        m_lyricsScrollTimer->stop();
    }
    m_audioEffectProcessor->reset();
    updatePlayButton();
    updatePlayingIndicators();
    // Playback failures are logged by the controller. Keep the current song available
    // for a manual retry without interrupting the user with a modal or tray message.
}

void MainWindow::toggleLibrary()
{
    const bool libraryVisible = m_contentStack->currentIndex() == 1;
    m_contentStack->setCurrentIndex(libraryVisible ? 0 : 1);
    m_libraryButton->setToolTip(libraryVisible ? QStringLiteral("音乐库与分组")
                                               : QStringLiteral("关闭音乐库"));
    m_libraryButton->setAccessibleName(m_libraryButton->toolTip());
    showIconButtonToolTip(m_libraryButton, true);
}

void MainWindow::togglePlayback()
{
    if (m_fileEditInProgress) {
        return;
    }
    if (m_playbackController->queue().isEmpty() && m_player->source().isEmpty() &&
        !m_songs.isEmpty()) {
        playRow(0);
    } else {
        m_playbackErrorShown = false;
        m_playbackController->toggle();
    }
}

void MainWindow::playPrevious()
{
    if (!m_fileEditInProgress) {
        m_playbackController->previous();
    }
}

void MainWindow::cyclePlayMode()
{
    const int mode = (static_cast<int>(m_playMode) + 1) % 4;
    if (!saveSetting(QStringLiteral("playMode"), QString::number(mode))) {
        return;
    }
    m_playMode = static_cast<PlayMode>(mode);
    updatePlayModeButton();
}

void MainWindow::updatePlayButton()
{
    const bool playing = m_playbackController != nullptr && m_playbackController->isPlaying();
    m_playButton->setIcon(
        QIcon(playing ? QStringLiteral(":/icons/pause.svg") : QStringLiteral(":/icons/play.svg")));
    m_playButton->setToolTip(playing ? QStringLiteral("暂停") : QStringLiteral("播放"));
    m_playButton->setAccessibleName(m_playButton->toolTip());
    showIconButtonToolTip(m_playButton, true);
    updateTaskbarToolbar();
}

void MainWindow::updatePlayModeButton()
{
    static const QStringList names = {QStringLiteral("顺序播放"), QStringLiteral("列表循环"),
                                      QStringLiteral("单曲循环"), QStringLiteral("随机播放")};
    static const QStringList icons = {
        QStringLiteral(":/icons/repeat_off.svg"), QStringLiteral(":/icons/repeat_all.svg"),
        QStringLiteral(":/icons/repeat_one.svg"), QStringLiteral(":/icons/shuffle.svg")};
    const int mode = qBound(0, static_cast<int>(m_playMode), 3);
    m_playMode = static_cast<PlayMode>(mode);
    m_playbackController->setMode(static_cast<PlaybackController::Mode>(mode));
    configureIconButton(m_modeButton, icons.at(mode), names.at(mode));
    showIconButtonToolTip(m_modeButton, true);
    updateTaskbarToolbar();
}

void MainWindow::applyAudioEffect(AudioEffectProcessor::Preset preset)
{
    const int presetValue = static_cast<int>(preset);
    if (presetValue < static_cast<int>(AudioEffectProcessor::Preset::Original) ||
        presetValue > static_cast<int>(AudioEffectProcessor::Preset::Live)) {
        preset = AudioEffectProcessor::Preset::Original;
    }
    if (m_playbackController != nullptr && m_playbackController->isFinishing()) {
        // Preserve the already processed tail. The selected setting is applied
        // when completion, recovery or a manual source change resets the output.
        m_pendingAudioEffectPreset = static_cast<int>(preset);
        saveSetting(QStringLiteral("audioEffect"), QString::number(static_cast<int>(preset)));
        updateAudioEffectButton();
        return;
    }
    m_pendingAudioEffectPreset = -1;
    m_audioEffectPreset = preset;
    if (m_audioEffectProcessor != nullptr) {
        m_audioEffectProcessor->setPreset(preset);
        m_audioEffectProcessor->setVolume(m_volume->value() / 100.0);
    }
    saveSetting(QStringLiteral("audioEffect"), QString::number(static_cast<int>(preset)));
    updateAudioEffectButton();
}

void MainWindow::updateAudioEffectButton()
{
    if (m_audioEffectButton == nullptr || m_audioEffectMenu == nullptr) {
        return;
    }
    QString effectName = QStringLiteral("原声");
    const int selectedPreset = m_pendingAudioEffectPreset >= 0
        ? m_pendingAudioEffectPreset : static_cast<int>(m_audioEffectPreset);
    for (QAction *action : m_audioEffectMenu->actions()) {
        const bool selected = action->data().toInt() == selectedPreset;
        action->setChecked(selected);
        if (selected) {
            effectName = action->text();
        }
    }
    m_audioEffectButton->setToolTip(effectName);
    m_audioEffectButton->setAccessibleName(m_audioEffectButton->toolTip());
}

void MainWindow::updateSongMetadata()
{
    if (m_player->source().isEmpty()) {
        return;
    }
    const QMediaMetaData metadata = m_player->metaData();
    const QString artist = artistFromMetadata(metadata);
    const QString album = metadata.stringValue(QMediaMetaData::AlbumTitle);
    m_artist->setText(artist.isEmpty() ? QStringLiteral("未知歌手") : artist);
    m_album->setText(album.isEmpty() ? QStringLiteral("未知专辑") : album);
    const qint64 songId = m_playbackController->currentId();
    if (!artist.isEmpty() && songId > 0 && !m_libraryImportInProgress && !m_fileEditInProgress) {
        QString error;
        if (m_repository->saveArtist(songId, artist, false, error)) {
            updateCachedArtist(songId, artist);
        } else if (!error.isEmpty()) {
            qWarning() << "Failed to save playing song artist:" << songId;
        }
    }
    const QVariant coverValue = metadata.value(QMediaMetaData::CoverArtImage);
    if (coverValue.canConvert<QImage>()) {
        const QImage coverImage = coverValue.value<QImage>();
        if (!coverImage.isNull()) {
            m_cover->setText({});
            m_cover->setPixmap(QPixmap::fromImage(coverImage)
                                   .scaled(m_cover->size(), Qt::KeepAspectRatioByExpanding,
                                           Qt::SmoothTransformation));
        }
    }
}

void MainWindow::updateCachedArtist(qint64 songId, const QString &artist)
{
    for (Song &song : m_librarySongs) {
        if (song.id == songId) {
            song.artist = artist;
            break;
        }
    }
    m_playbackController->updateArtist(songId, artist);
    for (int row = 0; row < m_songs.size(); ++row) {
        if (m_songs.at(row).id == songId) {
            m_songs[row].artist = artist;
            m_songModel->refreshArtist(row);
            break;
        }
    }
}

void MainWindow::scanSongsWithoutArtist(bool retryFailedScans)
{
    if (m_fileEditInProgress || m_libraryImportInProgress) {
        return;
    }
    for (const Song &song : std::as_const(m_librarySongs)) {
        const bool needsScan = song.artistScanState == ArtistScanState::Pending ||
                               (retryFailedScans &&
                                song.artistScanState == ArtistScanState::Failed);
        if (song.artist.isEmpty() && needsScan) {
            m_metadataScanner->enqueue(song.id, song.path);
        }
    }
}

void MainWindow::playNext(bool automatic)
{
    if (m_fileEditInProgress) {
        return;
    }
    if (m_playbackController->queue().isEmpty() && !automatic && !m_songs.isEmpty()) {
        playRow(0);
    } else {
        m_playbackController->next(automatic);
    }
}
