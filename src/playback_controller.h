#ifndef PLAYBACK_CONTROLLER_H
#define PLAYBACK_CONTROLLER_H

#include "audio_playback_progress.h"
#include "audio_player.h"
#include "music_types.h"

#include <QMediaPlayer>
#include <QObject>
#include <QSet>

#include <memory>

class QTimer;

class PlaybackController final : public QObject
{
    Q_OBJECT

public:
    enum class Mode
    {
        Sequential,
        ListLoop,
        SingleLoop,
        Random
    };

    explicit PlaybackController(AudioPlayer *player, QObject *parent = nullptr);
    ~PlaybackController() override;
    const QList<Song> &queue() const;
    qint64 currentId() const;
    int currentIndex() const;
    bool isPlaying() const;
    bool isFinishing() const;
    void setMode(Mode mode);
    void restoreQueue(const QList<Song> &songs);
    void synchronize(const QList<Song> &library);
    void updateArtist(qint64 songId, const QString &artist);
    void replaceAndPlay(const QList<Song> &songs);
    bool append(const Song &song, bool play);
    void playIndex(int index);
    void next(bool automatic = false);
    void previous();
    void toggle();
    void finishNaturalEnd(quint64 request);
    void cancelNaturalEnd();
    void seekTo(qint64 position);
    void failPlayback(const QString &reason);
    void recoverAudioOutput(const QString &reason);
    void remove(qint64 songId);
    void clear();
    void prepareFileEdit();
    void restoreFileEdit(const QString &path, qint64 position, QMediaPlayer::PlaybackState state);

signals:
    void queueChanged();
    void songStarted(const Song &song);
    void cleared();
    void playbackFailed(const QString &message);
    void naturalEndRequested(quint64 request);
    void outputResetRequested();
    void outputPauseRequested(bool paused);
    void playbackActivityChanged();
    void audioStreamChanged(quint64 stream);

private:
    int indexOf(qint64 songId) const;
    void beginAttempts(const QList<qint64> &ids, int historyTarget = -1);
    void tryNext();
    void acceptPlayingSong();
    void restoreWhenReady();
    void recoverWhenReady();
    void resetRecovery();
    void requestOutputDrain();
    void scheduleCompletion();
    void settleCompletion(quint64 request);
    void recoverPlayback();
    void watchErrorProgress();
    void resetAudioProgress(qint64 position);
    void invalidateAudioProgress();
    void loadCurrentSource();

    AudioPlayer *m_player;
    QTimer *m_loadTimeout;
    QTimer *m_errorProgressTimer;
    std::shared_ptr<AudioPlaybackProgress> m_audioProgress;
    quint64 m_bufferGeneration = 0;
    quint64 m_drainRevision = 0;
    bool m_outputDrained = false;
    bool m_decoderEnded = false;
    bool m_decoderFailed = false;
    bool m_completionQueued = false;
    bool m_completed = false;
    bool m_userPaused = false;
    QList<Song> m_queue;
    QList<qint64> m_history;
    int m_historyPosition = -1;
    int m_historyTarget = -1;
    qint64 m_currentId = 0;
    int m_removedPosition = 0;
    QList<qint64> m_attempts;
    QSet<qint64> m_tried;
    bool m_attemptPending = false;
    bool m_recordHistory = false;
    bool m_externalEdit = false;
    bool m_restoring = false;
    bool m_waitingForOutput = false;
    bool m_playbackFailed = false;
    bool m_changingSource = false;
    bool m_recoveryAttempted = false;
    bool m_recoveryLoading = false;
    bool m_recoverySeeking = false;
    bool m_recoveryResuming = false;
    qint64 m_recoveryPosition = 0;
    QString m_pendingPlaybackError;
    QMediaPlayer::Error m_pendingErrorCode = QMediaPlayer::NoError;
    Song m_currentSong;
    quint64 m_requestGeneration = 0;
    QString m_restorePath;
    qint64 m_restorePosition = 0;
    QMediaPlayer::PlaybackState m_restoreState = QMediaPlayer::StoppedState;
    Mode m_mode = Mode::ListLoop;
};

#endif
