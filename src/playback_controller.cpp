#include "playback_controller.h"
#include "music_file_utils.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>

#include <utility>

PlaybackController::PlaybackController(AudioPlayer *player, QObject *parent)
    : QObject(parent), m_player(player), m_loadTimeout(new QTimer(this)),
      m_errorProgressTimer(new QTimer(this)),
      m_audioProgress(std::make_shared<AudioPlaybackProgress>())
{
    connect(m_player, &AudioPlayer::audioBufferReceived, this,
            [progress = m_audioProgress](quint64 stream, const QAudioBuffer &buffer) {
                // 统计自带互斥保护；不跨线程访问控制器，销毁期间由共享引用保活。
                progress->receive(stream, buffer);
            }, Qt::DirectConnection);
    m_errorProgressTimer->setSingleShot(true);
    m_errorProgressTimer->setInterval(1500);
    connect(m_errorProgressTimer, &QTimer::timeout, this,
            &PlaybackController::watchErrorProgress);
    m_loadTimeout->setSingleShot(true);
    m_loadTimeout->setInterval(15000);
    connect(m_loadTimeout, &QTimer::timeout, this, [this] {
        if (m_recoveryLoading || m_recoveryResuming) {
            failPlayback(QStringLiteral("自动恢复播放超时，请重试或手动切换下一首。"));
        } else if (m_restoring) {
            failPlayback(QStringLiteral("歌曲已保存，但重新加载超时，请重试。"));
        } else if (m_attemptPending) {
            if (m_player->playbackState() == QMediaPlayer::PlayingState &&
                m_player->error() == QMediaPlayer::NoError) {
                acceptPlayingSong();
            } else {
                failPlayback(QStringLiteral("加载超过 15 秒仍未开始播放，请重试或手动切换下一首。"));
            }
        }
    });
    connect(m_player, &AudioPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                if (m_changingSource || m_playbackFailed || m_completed) {
                    return;
                }
                if (status == QMediaPlayer::EndOfMedia && !m_externalEdit &&
                    m_currentId > 0 && !m_recoveryLoading) {
                    // EOF and decoder errors can arrive in either order. Settle in a
                    // later event turn and never switch sources inside Qt's EOF callback.
                    m_decoderEnded = true;
                    m_recoveryResuming = false;
                    m_attemptPending = false;
                    m_loadTimeout->stop();
                    m_errorProgressTimer->stop();
                    if (m_audioProgress->snapshot().revision != m_drainRevision) {
                        m_outputDrained = false;
                    }
                    scheduleCompletion();
                    emit playbackActivityChanged();
                    return;
                }
                restoreWhenReady();
                recoverWhenReady();
                if (m_attemptPending && !m_externalEdit &&
                    m_player->playbackState() == QMediaPlayer::PlayingState) {
                    acceptPlayingSong();
                }
            });
    connect(m_player, &AudioPlayer::seekableChanged, this, [this](bool) {
        restoreWhenReady();
        recoverWhenReady();
    });
    connect(m_player, &AudioPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
                if (m_changingSource || m_playbackFailed) {
                    return;
                }
                if (state == QMediaPlayer::PlayingState && m_attemptPending &&
                    !m_externalEdit && !m_recoveryResuming) {
                    acceptPlayingSong();
                }
                emit playbackActivityChanged();
            });
    connect(m_player, &AudioPlayer::positionChanged, this, [this](qint64 position) {
        if (m_changingSource || m_playbackFailed || m_externalEdit) {
            return;
        }
        if (m_recoveryResuming && !m_recoverySeeking &&
            position > m_recoveryPosition && m_audioProgress->snapshot().frames > 0 &&
            m_player->playbackState() == QMediaPlayer::PlayingState &&
            m_player->error() == QMediaPlayer::NoError) {
            m_recoveryResuming = false;
            acceptPlayingSong();
            qInfo() << "Playback recovery resumed, song:" << m_currentId
                    << "position(ms):" << position;
        }
        if (position > 0 && m_attemptPending && !m_recoveryResuming &&
            m_player->playbackState() == QMediaPlayer::PlayingState) {
            acceptPlayingSong();
        }
    });
    connect(m_player, &AudioPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &description) {
                if (error == QMediaPlayer::NoError || m_currentId == 0 ||
                    m_changingSource || m_playbackFailed || m_completed) {
                    return;
                }
                if (m_externalEdit) {
                    if (m_restoring) {
                        const quint64 request = m_requestGeneration;
                        QTimer::singleShot(0, this, [this, request, description] {
                            if (request == m_requestGeneration && m_restoring) {
                                failPlayback(QStringLiteral("歌曲保存后重新加载失败：%1")
                                                 .arg(description));
                            }
                        });
                    }
                    return;
                }
                if (skipMissingSong()) {
                    return;
                }
                if (!m_pendingPlaybackError.isEmpty() &&
                    (m_pendingErrorCode != QMediaPlayer::FormatError ||
                     error == QMediaPlayer::FormatError)) {
                    return;
                }
                m_pendingPlaybackError = description.trimmed();
                if (m_pendingPlaybackError.isEmpty()) {
                    m_pendingPlaybackError = QStringLiteral("音频解码或读取失败");
                }
                m_pendingErrorCode = error;
                m_decoderFailed = true;
                // 解码器可能提前遇到坏数据。先保存恢复起点；后续若仍有有效
                // 音频输出，真正恢复时再推进到已排空的音频末端。
                m_recoveryPosition = qMax<qint64>(0, m_player->position());
                m_recoveryLoading = false;
                m_recoveryResuming = false;
                m_attemptPending = false;
                m_loadTimeout->stop();
                qWarning().noquote() << "Playback decoder error, song:" << m_currentId
                                    << "path:" << m_currentSong.path
                                    << "error:" << error << "position(ms):" << m_recoveryPosition
                                    << "duration(ms):" << m_player->duration()
                                    << "recovery attempted:" << m_recoveryAttempted
                                    << "reason:" << m_pendingPlaybackError;
                if (m_decoderEnded || m_player->mediaStatus() == QMediaPlayer::EndOfMedia) {
                    m_decoderEnded = true;
                    scheduleCompletion();
                } else if (!m_userPaused) {
                    m_errorProgressTimer->start();
                }
            });
}

const QList<Song> &PlaybackController::queue() const
{
    return m_queue;
}

qint64 PlaybackController::currentId() const
{
    return m_currentId;
}

int PlaybackController::currentIndex() const
{
    return indexOf(m_currentId);
}

bool PlaybackController::isPlaying() const
{
    return !m_userPaused && !m_playbackFailed && !m_completed && !m_externalEdit &&
           (m_player->playbackState() == QMediaPlayer::PlayingState || m_decoderEnded ||
            m_waitingForOutput || m_recoveryLoading || m_recoveryResuming);
}

bool PlaybackController::isFinishing() const
{
    return !m_completed && !m_playbackFailed &&
           (m_decoderEnded || m_waitingForOutput || m_outputDrained);
}

void PlaybackController::setMode(Mode mode)
{
    m_mode = mode;
}

int PlaybackController::indexOf(qint64 songId) const
{
    for (int index = 0; index < m_queue.size(); ++index) {
        if (m_queue.at(index).id == songId) {
            return index;
        }
    }
    return -1;
}

void PlaybackController::restoreQueue(const QList<Song> &songs)
{
    // Startup restoration only rebuilds the queue; audio is loaded on explicit playback.
    m_queue = songs;
    m_currentId = 0;
    m_removedPosition = 0;
    m_history.clear();
    m_historyPosition = -1;
    emit queueChanged();
}

PlaybackController::~PlaybackController()
{
    m_loadTimeout->stop();
    m_errorProgressTimer->stop();
    invalidateAudioProgress();
    disconnect(m_player, nullptr, this, nullptr);
    m_player->stop();
    m_player->setSource({});
}

void PlaybackController::synchronize(const QList<Song> &library)
{
    QHash<qint64, Song> byId;
    for (const Song &song : library) {
        byId.insert(song.id, song);
    }
    const int current = currentIndex();
    const int nextBoundary = current >= 0 ? current : m_removedPosition;
    int retainedBeforeBoundary = 0;
    QList<Song> updated;
    for (int index = 0; index < m_queue.size(); ++index) {
        const Song &song = m_queue.at(index);
        if (!byId.contains(song.id)) {
            continue;
        }
        updated.append(byId.value(song.id));
        if (index < nextBoundary) {
            ++retainedBeforeBoundary;
        }
    }
    // 当前歌曲移除后，边界表示剩余队列中的下一曲位置；后续删除也要同步移动边界。
    m_removedPosition = retainedBeforeBoundary;
    bool changed = updated.size() != m_queue.size();
    for (int index = 0; !changed && index < updated.size(); ++index) {
        const Song &before = m_queue.at(index);
        const Song &after = updated.at(index);
        changed = before.id != after.id || before.title != after.title || before.path != after.path;
    }
    m_queue = updated;
    if (changed) {
        emit queueChanged();
    }
}

void PlaybackController::updateArtist(qint64 songId, const QString &artist)
{
    const int index = indexOf(songId);
    if (index >= 0) {
        m_queue[index].artist = artist;
    }
}

void PlaybackController::replaceAndPlay(const QList<Song> &songs)
{
    if (songs.isEmpty()) {
        return;
    }
    m_queue = songs;
    m_removedPosition = 0;
    m_history.clear();
    m_historyPosition = -1;
    emit queueChanged();
    QList<qint64> ids;
    for (const Song &song : songs) {
        ids.append(song.id);
    }
    beginAttempts(ids);
}

bool PlaybackController::append(const Song &song, bool play)
{
    const int existing = indexOf(song.id);
    const bool added = existing < 0;
    const bool changed = added || m_queue.at(existing).title != song.title ||
                         m_queue.at(existing).path != song.path;
    if (added) {
        m_queue.append(song);
    } else {
        m_queue[existing] = song;
    }
    if (changed) {
        emit queueChanged();
    }
    if (play) {
        beginAttempts({song.id});
    }
    return added;
}

void PlaybackController::playIndex(int index)
{
    if (index >= 0 && index < m_queue.size()) {
        beginAttempts({m_queue.at(index).id});
    }
}

void PlaybackController::beginAttempts(const QList<qint64> &ids, int historyTarget)
{
    resetRecovery();
    ++m_requestGeneration;
    m_externalEdit = false;
    m_restoring = false;
    m_playbackFailed = false;
    m_userPaused = false;
    m_attempts = ids;
    m_tried.clear();
    m_historyTarget = historyTarget;
    m_recordHistory = true;
    m_attemptPending = true;
    tryNext();
}

void PlaybackController::tryNext()
{
    ++m_requestGeneration;
    m_loadTimeout->stop();
    while (!m_attempts.isEmpty()) {
        const qint64 id = m_attempts.takeFirst();
        const int index = indexOf(id);
        if (index < 0 || m_tried.contains(id)) {
            continue;
        }
        m_tried.insert(id);
        const Song song = m_queue.at(index);
        m_currentSong = song;
        m_currentId = id;
        QFile file(song.path);
        if (skipMissingSong()) {
            return;
        }
        if (!file.open(QIODevice::ReadOnly)) {
            failPlayback(QStringLiteral("无法读取文件：%1").arg(file.errorString()));
            return;
        }
        file.close();
        m_currentId = id;
        m_attemptPending = true;
        m_recordHistory = true;
        // 清空旧音源以确保重新播放同一首也从开头开始。
        invalidateAudioProgress();
        m_changingSource = true;
        m_player->stop();
        m_player->setSource({});
        m_changingSource = false;
        emit songStarted(song);
        m_loadTimeout->start();
        loadCurrentSource();
        m_player->play();
        return;
    }
    failPlayback(QStringLiteral("播放目标已不在当前列表中，请重新选择歌曲。"));
}

bool PlaybackController::skipMissingSong()
{
    if (m_currentId <= 0 || m_externalEdit || QFileInfo(m_currentSong.path).isFile()) {
        return false;
    }
    const Song missingSong = m_currentSong;
    const int index = currentIndex();
    qWarning().noquote() << "Skipping missing song:" << missingSong.id
                        << "path:" << missingSong.path;
    resetRecovery();
    invalidateAudioProgress();
    const quint64 request = ++m_requestGeneration;
    m_loadTimeout->stop();
    m_attemptPending = false;
    m_recordHistory = false;
    m_playbackFailed = false;
    m_attempts.clear();
    m_tried.clear();
    m_history.clear();
    m_historyPosition = -1;
    m_historyTarget = -1;
    m_changingSource = true;
    m_player->stop();
    m_player->setSource({});
    m_changingSource = false;
    emit outputResetRequested();
    if (index >= 0) {
        m_queue.removeAt(index);
        m_removedPosition = index;
    }
    m_currentId = 0;
    m_currentSong = {};
    // Delete the library record before persisting the updated queue. The database
    // cascades this deletion to every group membership.
    emit songFileMissing(missingSong);
    if (m_queue.isEmpty()) {
        clear();
        return true;
    }
    emit queueChanged();
    emit cleared();
    emit playbackActivityChanged();
    // Defer source changes out of decoder callbacks and avoid recursion when
    // several consecutive files are missing. A newer user action cancels this skip.
    QTimer::singleShot(0, this, [this, request] {
        if (request == m_requestGeneration) {
            next(false);
        }
    });
    return true;
}

void PlaybackController::acceptPlayingSong()
{
    if (m_player->playbackState() != QMediaPlayer::PlayingState ||
        m_player->error() != QMediaPlayer::NoError) {
        return;
    }
    m_loadTimeout->stop();
    m_attemptPending = false;
    // Once started, failures belong to this song and must never silently consume another candidate.
    m_attempts.clear();
    if (!m_recordHistory) {
        return;
    }
    m_recordHistory = false;
    while (m_historyTarget >= 0 && m_historyTarget < m_history.size() &&
           m_history.at(m_historyTarget) != m_currentId) {
        --m_historyTarget;
    }
    if (m_historyTarget >= 0 && m_historyTarget < m_history.size() &&
        m_history.at(m_historyTarget) == m_currentId) {
        m_historyPosition = m_historyTarget;
    } else {
        while (m_history.size() > m_historyPosition + 1) {
            m_history.removeLast();
        }
        if (m_history.isEmpty() || m_history.last() != m_currentId) {
            m_history.append(m_currentId);
        }
        m_historyPosition = m_history.size() - 1;
    }
}

void PlaybackController::next(bool automatic)
{
    if (m_externalEdit || m_queue.isEmpty()) {
        return;
    }
    QList<qint64> ids;
    const int current = currentIndex();
    if (automatic && m_mode == Mode::SingleLoop && current >= 0) {
        ids.append(m_currentId);
    }
    if (m_mode == Mode::Random) {
        for (const Song &song : std::as_const(m_queue)) {
            if (song.id != m_currentId || m_queue.size() == 1) {
                ids.append(song.id);
            }
        }
        for (int index = ids.size() - 1; index > 0; --index) {
            ids.swapItemsAt(index, QRandomGenerator::global()->bounded(index + 1));
        }
    } else {
        const int first = current >= 0 ? current + 1 : m_removedPosition;
        for (int index = first; index < m_queue.size(); ++index) {
            ids.append(m_queue.at(index).id);
        }
        if (!automatic || m_mode == Mode::ListLoop || m_mode == Mode::SingleLoop) {
            for (int index = 0; index < qMin(first, static_cast<int>(m_queue.size())); ++index) {
                ids.append(m_queue.at(index).id);
            }
        }
    }
    if (!ids.isEmpty()) {
        beginAttempts(ids);
    }
}

void PlaybackController::previous()
{
    if (m_externalEdit || m_queue.isEmpty()) {
        return;
    }
    const int count = static_cast<int>(m_queue.size());
    const int current = currentIndex();
    // 手动上一首始终使用列表顺序；当前歌曲移除时按原位置向前查找。
    const int boundary = current >= 0 ? current : qBound(0, m_removedPosition, count);
    const int target = boundary > 0 ? boundary - 1 : count - 1;
    playIndex(target);
}

void PlaybackController::toggle()
{
    if (m_externalEdit) {
        return;
    }
    if (m_completed || m_playbackFailed || m_player->source().isEmpty()) {
        if (!m_queue.isEmpty()) {
            playIndex(qMax(0, currentIndex()));
        }
        return;
    }
    if (isFinishing() || m_recoveryLoading || m_recoveryResuming) {
        m_userPaused = !m_userPaused;
        emit outputPauseRequested(m_userPaused);
        if (m_userPaused) {
            m_errorProgressTimer->stop();
            if (m_recoveryResuming) {
                m_player->pause();
                m_loadTimeout->stop();
            }
        } else {
            if (m_recoveryResuming) {
                m_loadTimeout->start();
                m_player->play();
            }
            scheduleCompletion();
            if (!m_pendingPlaybackError.isEmpty() && !m_decoderEnded) {
                m_errorProgressTimer->start();
            }
        }
        emit playbackActivityChanged();
        return;
    }
    m_userPaused = m_player->playbackState() == QMediaPlayer::PlayingState;
    emit outputPauseRequested(m_userPaused);
    if (m_userPaused) {
        m_errorProgressTimer->stop();
        m_player->pause();
    } else {
        m_player->play();
        if (!m_pendingPlaybackError.isEmpty()) {
            m_errorProgressTimer->start();
        }
    }
    emit playbackActivityChanged();
}

void PlaybackController::finishNaturalEnd(quint64 request)
{
    if (request != m_requestGeneration || !m_waitingForOutput || m_playbackFailed ||
        m_externalEdit || m_completed) {
        return;
    }
    m_waitingForOutput = false;
    m_outputDrained = true;
    scheduleCompletion();
}

void PlaybackController::requestOutputDrain()
{
    if (m_waitingForOutput || m_outputDrained || m_playbackFailed || m_userPaused ||
        m_completed) {
        return;
    }
    m_drainRevision = m_audioProgress->snapshot().revision;
    m_waitingForOutput = true;
    emit naturalEndRequested(m_requestGeneration);
}

void PlaybackController::scheduleCompletion()
{
    if (m_completionQueued || m_playbackFailed || m_completed || m_externalEdit) {
        return;
    }
    m_completionQueued = true;
    const quint64 request = m_requestGeneration;
    QTimer::singleShot(0, this, [this, request] {
        if (request != m_requestGeneration) {
            return;
        }
        m_completionQueued = false;
        settleCompletion(request);
    });
}

void PlaybackController::settleCompletion(quint64 request)
{
    if (request != m_requestGeneration || m_playbackFailed || m_completed ||
        m_externalEdit || m_userPaused || m_recoveryLoading || m_recoveryResuming ||
        m_waitingForOutput || (!m_decoderEnded && m_pendingPlaybackError.isEmpty())) {
        return;
    }
    const auto audio = m_audioProgress->snapshot();
    if (m_outputDrained && audio.revision != m_drainRevision) {
        // PCM can arrive after a stalled decoder's first drain. Never discard it
        // or apply an earlier drain acknowledgement to a later input interval.
        m_outputDrained = false;
        if (!m_decoderEnded && audio.ageMs < 1500) {
            m_errorProgressTimer->start();
            return;
        }
    }
    if (!m_outputDrained) {
        requestOutputDrain();
        return;
    }
    const auto error = m_pendingErrorCode != QMediaPlayer::NoError
        ? m_pendingErrorCode : m_player->error();
    // Local damage is handled inside the streaming decoder. Only its real EOF,
    // followed by the audible sink's drain acknowledgement, completes a song.
    const bool ordinaryEnd = m_decoderEnded && audio.ended &&
        !m_decoderFailed && error == QMediaPlayer::NoError;
    if (ordinaryEnd) {
        qInfo().noquote() << "Playback completed, song:" << m_currentId
                          << "mode:" << static_cast<int>(m_mode)
                          << "decoder warning:" << (error != QMediaPlayer::NoError)
                          << audio.description();
        m_completed = true;
        invalidateAudioProgress();
        m_decoderEnded = false;
        m_outputDrained = false;
        m_pendingPlaybackError.clear();
        m_pendingErrorCode = QMediaPlayer::NoError;
        m_errorProgressTimer->stop();
        m_loadTimeout->stop();
        emit outputResetRequested();
        emit playbackActivityChanged();
        // All modes share this single completed transition. Sequential playback
        // at the final row stops; single-loop and random still start a fresh request.
        next(true);
        return;
    }
    recoverPlayback();
}

void PlaybackController::watchErrorProgress()
{
    if (m_pendingPlaybackError.isEmpty() || m_playbackFailed || m_completed ||
        m_externalEdit || m_userPaused) {
        return;
    }
    if (m_decoderEnded) {
        scheduleCompletion();
        return;
    }
    const auto audio = m_audioProgress->snapshot();
    const bool bufferedAhead = audio.endTimeUs() > m_player->position() * 1000;
    if (m_player->playbackState() == QMediaPlayer::PlayingState &&
        (audio.ageMs < 1500 || bufferedAhead)) {
        // Large codec frames can contain seconds of valid PCM. Allow that known
        // audio to play before deciding that a lack of new buffers is a stall.
        // Position alone is never used as evidence for completing a song.
        m_errorProgressTimer->start();
        return;
    }
    requestOutputDrain();
}

void PlaybackController::invalidateAudioProgress()
{
    m_player->setAudioStream(0);
    m_audioProgress->invalidate();
    emit audioStreamChanged(0);
}

void PlaybackController::resetAudioProgress(qint64 position)
{
    const quint64 stream = ++m_bufferGeneration;
    m_audioProgress->reset(stream, position);
    emit audioStreamChanged(stream);
    m_player->setAudioStream(stream);
}

void PlaybackController::loadCurrentSource()
{
    emit outputResetRequested();
    resetAudioProgress(0);
    m_player->setSource(QUrl::fromLocalFile(m_currentSong.path));
}

void PlaybackController::recoverAudioOutput(const QString &reason)
{
    if (m_playbackFailed || m_externalEdit || m_completed) {
        return;
    }
    if (m_decoderFailed) {
        failPlayback(reason);
        return;
    }
    // A stopped device may still have owned buffered audio. Replay a short
    // overlap on recovery rather than jumping past data that was never audible.
    constexpr qint64 OutputRecoveryOverlapMs = 2000;
    m_recoveryPosition = qMax<qint64>(0, m_player->position() - OutputRecoveryOverlapMs);
    m_pendingPlaybackError = reason;
    m_pendingErrorCode = QMediaPlayer::ResourceError;
    m_recoveryLoading = false;
    m_recoveryResuming = false;
    m_loadTimeout->stop();
    m_errorProgressTimer->stop();
    m_decoderEnded = false;
    m_waitingForOutput = false;
    m_outputDrained = true;
    m_drainRevision = m_audioProgress->snapshot().revision;
    qWarning().noquote() << "Playback audio output recovery, song:" << m_currentId
                        << "reason:" << reason;
    if (!m_userPaused) {
        recoverPlayback();
    }
}

void PlaybackController::recoverPlayback()
{
    if (m_playbackFailed || m_recoveryLoading || m_externalEdit || m_userPaused) {
        return;
    }
    const auto audio = m_audioProgress->snapshot();
    if (m_decoderFailed) {
        // The worker has already exhausted local recovery. Reloading at an old
        // error position would replay valid audio that has now been drained.
        failPlayback(m_pendingPlaybackError);
        return;
    }
    if (m_recoveryAttempted) {
        failPlayback(QStringLiteral("自动恢复后仍无法继续播放：%1；%2")
                         .arg(m_pendingPlaybackError, audio.description()));
        return;
    }
    m_recoveryAttempted = true;
    m_recoveryLoading = true;
    m_outputDrained = false;
    m_decoderEnded = false;
    m_waitingForOutput = false;
    m_completionQueued = false;
    m_pendingPlaybackError.clear();
    m_pendingErrorCode = QMediaPlayer::NoError;
    m_errorProgressTimer->stop();
    ++m_requestGeneration;
    qInfo().noquote() << "Playback recovery loading, song:" << m_currentId
                      << "position(ms):" << m_recoveryPosition << audio.description();
    invalidateAudioProgress();
    m_changingSource = true;
    m_player->stop();
    m_player->setSource({});
    m_changingSource = false;
    m_loadTimeout->start();
    loadCurrentSource();
    recoverWhenReady();
}

void PlaybackController::recoverWhenReady()
{
    if (!m_recoveryLoading || m_changingSource || m_playbackFailed ||
        m_player->error() != QMediaPlayer::NoError || !m_player->isSeekable() ||
        normalizedPath(m_player->source().toLocalFile()) != normalizedPath(m_currentSong.path) ||
        (m_player->mediaStatus() != QMediaPlayer::LoadedMedia &&
         m_player->mediaStatus() != QMediaPlayer::BufferedMedia)) {
        return;
    }
    if (m_player->duration() > 0 && m_player->duration() < m_recoveryPosition) {
        failPlayback(QStringLiteral("恢复位置已超出音频范围，请重试或手动切歌。"));
        return;
    }
    m_recoveryLoading = false;
    m_recoveryResuming = !m_userPaused;
    m_recoverySeeking = true;
    m_player->setPosition(m_recoveryPosition);
    resetAudioProgress(m_recoveryPosition);
    m_recoverySeeking = false;
    emit outputPauseRequested(m_userPaused);
    if (m_userPaused) {
        m_loadTimeout->stop();
    } else if (m_player->error() == QMediaPlayer::NoError) {
        m_player->play();
    }
    emit playbackActivityChanged();
}

void PlaybackController::resetRecovery()
{
    m_errorProgressTimer->stop();
    m_waitingForOutput = false;
    m_outputDrained = false;
    m_decoderEnded = false;
    m_decoderFailed = false;
    m_completionQueued = false;
    m_completed = false;
    m_recoveryAttempted = false;
    m_recoveryLoading = false;
    m_recoverySeeking = false;
    m_recoveryResuming = false;
    m_recoveryPosition = 0;
    m_drainRevision = 0;
    m_pendingPlaybackError.clear();
    m_pendingErrorCode = QMediaPlayer::NoError;
}

void PlaybackController::cancelNaturalEnd()
{
    ++m_requestGeneration;
    resetRecovery();
}

void PlaybackController::seekTo(qint64 position)
{
    if (m_currentId <= 0 || m_externalEdit || m_player->source().isEmpty()) {
        return;
    }
    position = qMax<qint64>(0, position);
    if (m_player->duration() > 0) {
        position = qMin(position, m_player->duration());
    }
    const bool playing = isPlaying();
    const bool reload = m_recoveryLoading || m_recoveryResuming || m_playbackFailed ||
                        m_player->error() != QMediaPlayer::NoError;
    invalidateAudioProgress();
    cancelNaturalEnd();
    m_playbackFailed = false;
    m_userPaused = !playing;
    m_attemptPending = false;
    m_loadTimeout->stop();
    if (reload) {
        m_recoveryPosition = position;
        m_recoveryLoading = true;
        m_changingSource = true;
        m_player->stop();
        m_player->setSource({});
        m_changingSource = false;
        m_loadTimeout->start();
        loadCurrentSource();
        recoverWhenReady();
        return;
    }
    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->pause();
    }
    emit outputResetRequested();
    m_player->setPosition(position);
    resetAudioProgress(position);
    emit outputPauseRequested(m_userPaused);
    if (playing) {
        m_player->play();
    }
    emit playbackActivityChanged();
}

void PlaybackController::failPlayback(const QString &reason)
{
    if (m_playbackFailed) {
        return;
    }
    if (skipMissingSong()) {
        return;
    }
    m_playbackFailed = true;
    resetRecovery();
    invalidateAudioProgress();
    ++m_requestGeneration;
    m_waitingForOutput = false;
    m_attemptPending = false;
    m_recordHistory = false;
    m_externalEdit = false;
    m_restoring = false;
    m_attempts.clear();
    m_loadTimeout->stop();
    const QString path = m_currentSong.path;
    const QString title = m_currentSong.title.isEmpty() ? QFileInfo(path).completeBaseName()
                                                       : m_currentSong.title;
    const QString message = QStringLiteral("歌曲：%1\n文件：%2\n原因：%3\n\n可重试或手动切换下一首。")
                                .arg(title, path, reason);
    qWarning().noquote() << "Playback failed, song:" << m_currentId << "path:" << path
                        << "position(ms):" << m_player->position() << "reason:" << reason;
    m_player->stop();
    emit outputResetRequested();
    emit playbackActivityChanged();
    emit playbackFailed(message);
}

void PlaybackController::remove(qint64 songId)
{
    const int index = indexOf(songId);
    if (index < 0) {
        return;
    }
    const bool current = songId == m_currentId;
    if (currentIndex() < 0 && index < m_removedPosition) {
        --m_removedPosition;
    }
    m_queue.removeAt(index);
    emit queueChanged();
    if (current) {
        if (m_queue.isEmpty()) {
            clear();
        } else {
            const int first = index < m_queue.size() ? index : 0;
            QList<qint64> ids;
            for (int offset = 0; offset < m_queue.size(); ++offset) {
                ids.append(m_queue.at((first + offset) % m_queue.size()).id);
            }
            beginAttempts(ids);
        }
    }
}

void PlaybackController::clear()
{
    resetRecovery();
    invalidateAudioProgress();
    m_userPaused = false;
    m_waitingForOutput = false;
    m_playbackFailed = false;
    m_currentSong = {};
    m_externalEdit = false;
    m_restoring = false;
    ++m_requestGeneration;
    m_loadTimeout->stop();
    m_attemptPending = false;
    m_attempts.clear();
    m_history.clear();
    m_historyPosition = -1;
    m_currentId = 0;
    m_removedPosition = 0;
    m_recordHistory = false;
    m_queue.clear();
    m_player->stop();
    m_player->setSource({});
    emit outputResetRequested();
    emit playbackActivityChanged();
    emit queueChanged();
    emit cleared();
}

void PlaybackController::prepareFileEdit()
{
    resetRecovery();
    invalidateAudioProgress();
    m_waitingForOutput = false;
    ++m_requestGeneration;
    m_externalEdit = true;
    m_attemptPending = false;
    m_attempts.clear();
    m_loadTimeout->stop();
    m_player->stop();
    m_player->setSource({});
    emit outputResetRequested();
}

void PlaybackController::restoreFileEdit(const QString &path, qint64 position,
                                         QMediaPlayer::PlaybackState state)
{
    m_playbackFailed = false;
    m_currentSong.path = path;
    m_currentSong.title = QFileInfo(path).completeBaseName();
    m_restorePath = path;
    m_restorePosition = position;
    m_restoreState = state;
    m_userPaused = state != QMediaPlayer::PlayingState;
    m_restoring = true;
    m_loadTimeout->start();
    loadCurrentSource();
    restoreWhenReady();
}

void PlaybackController::restoreWhenReady()
{
    if (!m_restoring || !m_player->isSeekable() ||
        normalizedPath(m_player->source().toLocalFile()) != normalizedPath(m_restorePath) ||
        (m_player->mediaStatus() != QMediaPlayer::LoadedMedia &&
         m_player->mediaStatus() != QMediaPlayer::BufferedMedia)) {
        return;
    }
    m_restoring = false;
    m_externalEdit = false;
    m_loadTimeout->stop();
    const qint64 position = qBound<qint64>(0, m_restorePosition, m_player->duration());
    m_player->setPosition(position);
    resetAudioProgress(position);
    emit outputPauseRequested(m_userPaused);
    if (m_restoreState == QMediaPlayer::PlayingState) {
        m_player->play();
    } else if (m_restoreState == QMediaPlayer::PausedState) {
        m_player->play();
        m_player->pause();
    }
    emit playbackActivityChanged();
}
