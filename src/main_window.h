#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include "audio_effect_processor.h"
#include "audio_player.h"
#include "music_types.h"
#include "audio_file_editor.h"
#include "lyric_parser.h"

#include <QFuture>
#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QSqlDatabase>

#include <functional>
#include <memory>

class QCloseEvent;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QNetworkAccessManager;
class QPropertyAnimation;
class QMenu;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QSystemTrayIcon;
class QTableView;
class QThread;
class QTimer;

#ifdef Q_OS_WIN
struct ITaskbarList3;
#endif

class MusicLibraryRepository;
class PlaybackController;
class MetadataScanner;
class ShortcutManager;
class DesktopLyricsWindow;

namespace PlayerWidgets {
class SongTableModel;
}

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

private:
    enum class PlayMode
    {
        Sequential,
        ListLoop,
        SingleLoop,
        Random
    };

    void buildUi();
    void createTitleBar();
    QWidget *createLibraryPage();
    QWidget *createNowPlayingPage();
    QWidget *createSettingsPage();
    QWidget *createShortcutSettingsPage();
    void initializeShortcuts();
    void initializeDesktopLyrics();
    void toggleDesktopLyrics();
    void updateDesktopLyrics();
    void createPlayerBar();
    void initializePlayback();
    void connectLibraryControls();
    void initializeSystemTray();
    void updateTrayPlayback();
    void restoreFromTray();
    void initializeTaskbarToolbar();
    void updateTaskbarToolbar();
    void updateTaskbarBackground();
#ifdef Q_OS_WIN
    bool publishTaskbarBackground(const QSize &maximumSize, bool livePreview);
#endif
    bool initializeDatabase();
    void loadGroups();
    void loadSongs(bool reloadLibrary = true);
    void refreshFileSizeCache();
    void synchronizePlaybackQueue();
    void restorePlaybackQueue();
    void savePlaybackQueue();
    void rebuildPlaybackQueue();
    void updatePlayingIndicators();
    void refreshMusicLibrary(bool retryFailedScans = false);
    void updateLibraryRefreshProgress(int completed, int total);
    void addFiles(qint64 groupId);
    void addFolder(qint64 groupId);
    void importMusic(const QStringList &files, const QString &folder, qint64 groupId);
    void createGroup();
    void renameGroupInline(qint64 groupId, const QString &currentName);
    void showGroupEditorError(QLineEdit *editor, const QString &message);
    void addGroupItem(qint64 groupId, const QString &name);
    void showGroupMenu(qint64 groupId, const QString &name, QWidget *anchor);
    void showSongMenu(const QPoint &position);
    void showQueueMenu(const QPoint &position);
    void editSongMetadata(qint64 songId);
    void showMetadataEditor(const AudioMetadataSnapshot &snapshot);
    void playGroup(qint64 groupId);
    void addSongToPlaybackQueue(qint64 songId);
    void playRow(int row);
    void playQueueRow(int row);
    void showPlayingSong(const Song &song);
    void clearPlayingSong();
    void startAudioFileEdit(const AudioEditRequest &request,
                            const std::function<void(const AudioEditResult &, bool)> &completion);
    void reportPlaybackError(const QString &message);
    void togglePlayback();
    void playPrevious();
    void cyclePlayMode();
    void playNext(bool automatic = false);
    void chooseBackground();
    void applyBackground(const QString &path);
    quint64 m_backgroundUpdateGeneration = 0;
    void showSettings();
    void applyFonts(const QString &applicationFont, int applicationFontSize,
                    const QString &lyricsFont, int lyricsFontSize);
    void toggleMaximized();
    void updateMaximizeButton();
    void updateLyricsPalette(const QString &backgroundPath);
    void updateLyricsScriptButtonPalette(const QString &backgroundPath);
    void updateCaptionButtonPalette(const QString &backgroundPath);
    void toggleQueueDrawer();
    void filterPlaybackQueue();
    void updateQueueDrawerGeometry();
    QRect queueDrawerRect(bool open) const;
    void updateSongInfoPalette(const QString &backgroundPath);
    void updateDialogTheme(const QString &backgroundPath);
    void toggleLibrary();
    void loadLyrics(const QString &audioPath);
    QString displayedLyricsText(const QString &text) const;
    void updateLyricsScriptButton();
    void showMissingLyrics(const QString &audioPath);
    void requestLyricsDownload(const QString &audioPath);
    bool loadTimestampedLyrics(const QString &content);
    QString embeddedLyrics(const QString &audioPath, bool synchronized) const;
    void showPlainLyrics(const QString &content);
    void updateLyrics(qint64 position);
    void updateSongMetadata();
    void updateCachedArtist(qint64 songId, const QString &artist);
    void scanSongsWithoutArtist(bool retryFailedScans = false);
    void updatePlayButton();
    void updatePlayModeButton();
    void applyAudioEffect(AudioEffectProcessor::Preset preset);
    void updateAudioEffectButton();
    qint64 currentGroupId() const;
    qint64 currentSongId() const;
    QString setting(const QString &key, const QString &fallback = {}) const;
    bool saveSetting(const QString &key, const QString &value, bool showError = true);
    bool saveSettings(const QList<QPair<QString, QString>> &values);
    static QString formatTime(qint64 milliseconds);

    QSqlDatabase m_database;
    std::unique_ptr<MusicLibraryRepository> m_repository;
    ShortcutManager *m_shortcutManager = nullptr;
    DesktopLyricsWindow *m_desktopLyrics = nullptr;
    QTimer *m_desktopLyricsTimer = nullptr;
    int m_volumeBeforeMute = 70;
    PlaybackController *m_playbackController = nullptr;
    QSystemTrayIcon *m_trayIcon = nullptr;
    QLabel *m_traySongLabel = nullptr;
    QPushButton *m_trayPreviousButton = nullptr;
    QPushButton *m_trayPlayButton = nullptr;
    QPushButton *m_trayNextButton = nullptr;
    QPushButton *m_trayModeButton = nullptr;
    QPushButton *m_trayDesktopLyricsButton = nullptr;
    bool m_exitRequested = false;
#ifdef Q_OS_WIN
    ITaskbarList3 *m_taskbar = nullptr;
    unsigned int m_taskbarCreatedMessage = 0;
    bool m_taskbarButtonsAdded = false;
    bool m_taskbarComInitialized = false;
    QImage m_taskbarBackground;
    QString m_taskbarBackgroundPath;
#endif
    AudioPlayer *m_player = nullptr;
    MetadataScanner *m_metadataScanner = nullptr;
    QNetworkAccessManager *m_networkManager = nullptr;
    AudioEffectProcessor *m_audioEffectProcessor = nullptr;
    quint64 m_audioDrainRequest = 0;
    int m_pendingAudioEffectPreset = -1;
    QThread *m_audioEffectThread = nullptr;
    QListWidget *m_groups = nullptr;
    QListWidget *m_queue = nullptr;
    QListWidget *m_lyrics = nullptr;
    QTableView *m_songsTable = nullptr;
    PlayerWidgets::SongTableModel *m_songModel = nullptr;
    QLabel *m_cover = nullptr;
    QLabel *m_detailTitle = nullptr;
    QLabel *m_artist = nullptr;
    QLabel *m_album = nullptr;
    QLabel *m_time = nullptr;
    QWidget *m_titleBar = nullptr;
    QWidget *m_playerBar = nullptr;
    QWidget *m_queueDrawer = nullptr;
    QStackedWidget *m_contentStack = nullptr;
    QPushButton *m_libraryButton = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_modeButton = nullptr;
    QPushButton *m_refreshLibraryButton = nullptr;
    QPushButton *m_lyricsScriptButton = nullptr;
    QPushButton *m_desktopLyricsButton = nullptr;
    QPushButton *m_audioEffectButton = nullptr;
    QPushButton *m_queueButton = nullptr;
    QPushButton *m_maximizeButton = nullptr;
    QList<QPushButton *> m_captionButtons;
    QFontComboBox *m_applicationFontSelector = nullptr;
    QFontComboBox *m_lyricsFontSelector = nullptr;
    QSpinBox *m_applicationFontSizeSelector = nullptr;
    QSpinBox *m_lyricsFontSizeSelector = nullptr;
    QTimer *m_lyricsButtonPaletteTimer = nullptr;
    QPropertyAnimation *m_queueDrawerAnimation = nullptr;
    QMenu *m_audioEffectMenu = nullptr;
    QLineEdit *m_backgroundEditor = nullptr;
    QLineEdit *m_searchEditor = nullptr;
    QLineEdit *m_queueSearchEditor = nullptr;
    QTimer *m_searchTimer = nullptr;
    QTimer *m_volumeSaveTimer = nullptr;
    QTimer *m_lyricsScrollTimer = nullptr;
    bool m_lyricsViewDirty = true;
    QSlider *m_progress = nullptr;
    QSlider *m_volume = nullptr;
    QLineEdit *m_groupEditor = nullptr;
    QList<Song> m_songs;
    QList<Song> m_librarySongs;
    QHash<QString, qint64> m_fileSizeCache;
    QFuture<void> m_importFuture;
    QFuture<void> m_refreshFuture;
    QFuture<void> m_editFuture;
    QFuture<void> m_metadataReadFuture;
    bool m_metadataReadInProgress = false;
    bool m_fileEditInProgress = false;
    int m_fileSizeGeneration = 0;
    bool m_libraryImportInProgress = false;
    bool m_playbackErrorShown = false;
    QList<Lyrics::Line> m_lyricLines;
    QString m_missingLyricsAudioPath;
    QString m_backgroundPath;
    QString m_baseStyleSheet;
    QString m_dialogThemeStyle;
    qint64 m_activeGroupId = 0;
    int m_currentLyricIndex = -1;
    int m_settingsReturnIndex = 0;
    bool m_libraryRefreshInProgress = false;
    bool m_queueDrawerOpen = false;
    bool m_playbackQueueRestored = false;
    QString m_savedPlaybackQueue;
    bool m_showSimplifiedLyrics = true;
    AudioEffectProcessor::Preset m_audioEffectPreset = AudioEffectProcessor::Preset::Original;
    PlayMode m_playMode = PlayMode::ListLoop;
};

#endif // MAIN_WINDOW_H
