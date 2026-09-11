#include "main_window.h"
#include "player_widgets.h"
#include "music_library_repository.h"
#include "playback_controller.h"
#include "metadata_scanner.h"

#include <QDebug>
#include <QListWidget>
#include <QMediaPlayer>
#include <QNetworkAccessManager>
#include <QSlider>
#include <QThread>
#include <QTimer>
#include <QVariant>

using namespace PlayerWidgets;

void MainWindow::initializePlayback()
{
    m_player = new AudioPlayer(this);
    m_queue->setItemDelegate(new QueueItemDelegate(m_queue, m_player));
    m_playbackController = new PlaybackController(m_player, this);
    m_metadataScanner = new MetadataScanner(this);
    connect(m_playbackController, &PlaybackController::songStarted, this,
            &MainWindow::showPlayingSong);
    connect(m_playbackController, &PlaybackController::cleared, this,
            &MainWindow::clearPlayingSong);
    connect(m_playbackController, &PlaybackController::outputResetRequested, this, [this] {
        m_audioDrainRequest = 0;
        if (m_audioEffectProcessor != nullptr) {
            m_audioEffectProcessor->reset();
        }
        if (m_pendingAudioEffectPreset >= 0) {
            const auto preset = static_cast<AudioEffectProcessor::Preset>(m_pendingAudioEffectPreset);
            m_pendingAudioEffectPreset = -1;
            applyAudioEffect(preset);
        }
    });
    connect(m_playbackController, &PlaybackController::playbackFailed, this,
            &MainWindow::reportPlaybackError);
    connect(m_playbackController, &PlaybackController::queueChanged, this, [this] {
        rebuildPlaybackQueue();
        updateTaskbarToolbar();
        savePlaybackQueue();
    });
    m_networkManager = new QNetworkAccessManager(this);
    // Every preset shares one FFmpeg decoder and one audible output device.
    m_audioEffectThread = new QThread(this);
    m_audioEffectThread->setObjectName(QStringLiteral("AudioEffectThread"));
    m_audioEffectProcessor = new AudioEffectProcessor;
    m_audioEffectProcessor->moveToThread(m_audioEffectThread);
    connect(m_audioEffectThread, &QThread::finished, m_audioEffectProcessor, &QObject::deleteLater);
    connect(m_playbackController, &PlaybackController::audioStreamChanged,
            m_audioEffectProcessor, &AudioEffectProcessor::setStream, Qt::DirectConnection);
    connect(m_player, &AudioPlayer::audioBufferReceived,
            m_audioEffectProcessor, &AudioEffectProcessor::processBuffer, Qt::DirectConnection);
    connect(m_playbackController, &PlaybackController::outputPauseRequested,
            m_audioEffectProcessor, &AudioEffectProcessor::setPaused, Qt::DirectConnection);
    connect(m_playbackController, &PlaybackController::playbackActivityChanged, this, [this] {
        updatePlayButton();
        updatePlayingIndicators();
    });
    connect(m_audioEffectProcessor, &AudioEffectProcessor::outputFailed, this,
            [this](quint64 generation, const QString &reason) {
                if (generation != m_audioEffectProcessor->generation() ||
                    m_playbackController == nullptr) {
                    return;
                }
                m_audioDrainRequest = 0;
                m_playbackController->recoverAudioOutput(reason);
            }, Qt::QueuedConnection);
    connect(m_playbackController, &PlaybackController::naturalEndRequested, this,
            [this](quint64 request) {
                m_audioDrainRequest = request;
                m_audioEffectProcessor->finishStream(request);
            });
    connect(m_audioEffectProcessor, &AudioEffectProcessor::outputDrained, this,
            [this](quint64 request, quint64 generation) {
                if (request != m_audioDrainRequest ||
                    generation != m_audioEffectProcessor->generation()) {
                    return;
                }
                m_audioDrainRequest = 0;
                // Keep the sink alive until the controller settles EOF/recovery.
                // Late PCM must not be erased by an early drain acknowledgement.
                m_playbackController->finishNaturalEnd(request);
            }, Qt::QueuedConnection);
    connect(m_audioEffectProcessor, &AudioEffectProcessor::outputProgress, m_player,
            &AudioPlayer::reportOutputProgress, Qt::DirectConnection);
    m_audioEffectThread->start();
    connect(m_player, &AudioPlayer::sourceChanged, this, [this] {
        updateTaskbarToolbar();
        updatePlayingIndicators();
    });
    m_volumeSaveTimer = new QTimer(this);
    m_volumeSaveTimer->setSingleShot(true);
    m_volumeSaveTimer->setInterval(300);
    connect(m_volumeSaveTimer, &QTimer::timeout, this, [this] {
        if (m_libraryImportInProgress || m_fileEditInProgress) {
            m_volumeSaveTimer->start();
            return;
        }
        saveSetting(QStringLiteral("volume"), QString::number(m_volume->value()));
    });
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        const qreal volume = value / 100.0;
        m_audioEffectProcessor->setVolume(volume);
        m_volumeSaveTimer->start();
    });
    connect(m_progress, &QSlider::sliderMoved, this, [this](int position) {
        m_playbackController->seekTo(position);
    });
    connect(m_player, &AudioPlayer::durationChanged, this, [this](qint64 duration) {
        m_progress->setRange(0, static_cast<int>(duration));
    });
    connect(m_player, &AudioPlayer::positionChanged, this, [this](qint64 position) {
        if (!m_progress->isSliderDown()) {
            m_progress->setValue(static_cast<int>(position));
        }
        m_time->setText(formatTime(position) + QStringLiteral(" / ") +
                        formatTime(m_player->duration()));
        updateLyrics(position);
    });
    connect(m_player, &AudioPlayer::metaDataChanged, this, &MainWindow::updateSongMetadata);
    connect(m_metadataScanner, &MetadataScanner::scanFinished, this,
            [this](qint64 songId, const QString &artist, bool succeeded) {
                if (m_fileEditInProgress || m_libraryImportInProgress) {
                    return;
                }
                QString error;
                const ArtistScanState state = succeeded ? ArtistScanState::Completed
                                                        : ArtistScanState::Failed;
                if (m_repository->saveArtistScan(songId, artist, state, error)) {
                    bool updateArtist = false;
                    for (Song &song : m_librarySongs) {
                        if (song.id == songId) {
                            song.artistScanState = state;
                            updateArtist = song.artist.isEmpty() && !artist.isEmpty();
                            break;
                        }
                    }
                    if (updateArtist) {
                        updateCachedArtist(songId, artist);
                    }
                } else if (!error.isEmpty()) {
                    qWarning() << "Failed to save artist scan result:" << songId;
                }
            });
    m_lyricsScrollTimer = new QTimer(this);
    m_lyricsScrollTimer->setSingleShot(true);
    m_lyricsScrollTimer->setTimerType(Qt::PreciseTimer);
    connect(m_lyricsScrollTimer, &QTimer::timeout, this, [this] {
        updateLyrics(m_player->position());
    });
    connect(m_player, &AudioPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
                updateLyrics(m_player->position());
                updatePlayButton();
                updatePlayingIndicators();
                if (state == QMediaPlayer::PlayingState) {
                    m_audioEffectProcessor->setPaused(false);
                } else if (state == QMediaPlayer::PausedState) {
                    m_audioEffectProcessor->setPaused(true);
                }
                // Decoder EOF can precede the last audible samples. The
                // controller keeps that output alive until the sink drains.
            });
}
