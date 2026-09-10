#ifndef LYRICS_DOWNLOAD_DIALOG_H
#define LYRICS_DOWNLOAD_DIALOG_H

#include "app_dialog.h"

#include <QElapsedTimer>
#include <QList>
#include <QPointer>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QUrl;
class QJsonDocument;

class LyricsDownloadDialog final : public AppDialog
{
    Q_OBJECT

public:
    LyricsDownloadDialog(const QString &audioPath, QNetworkAccessManager *networkManager,
                         QWidget *backgroundSurface, const QPixmap &backdrop, QWidget *parent);
    ~LyricsDownloadDialog() override;
    void reject() override;
    void finishEmbeddedSave(bool success, bool needsOverwrite, const QString &message);

signals:
    void lyricsSaved(const QString &audioPath);
    void embeddedSaveRequested(const QString &audioPath, const QString &lyrics,
                               bool overwriteConfirmed);

private:
    struct LyricResult
    {
        QString title;
        QString artist;
        QString album;
        QString lyrics;
        /** 接口中的歌曲时长，单位为秒；负数表示缺失或无效。 */
        double durationSeconds = -1.0;
        /** 是否包含同步歌词，用于逐句滚动显示和结果优先排序。 */
        bool synchronized = false;
    };

    enum class StatusTone
    {
        Neutral,
        Error,
        Success
    };

    enum class Source
    {
        Automatic,
        Lrclib,
        Kugou
    };

    enum class SearchStage
    {
        PrimarySearch,
        BackupSearch,
        BackupLyrics
    };

    struct BackupCandidate
    {
        LyricResult result;
        QString id;
        QString accessKey;
    };

    void startSearch();
    void cancelSearch();
    void finishSearch(QNetworkReply *reply, quint64 generation);
    void sendSearchRequest(const QUrl &url);
    void handleSearchFailure(const QString &reason);
    void startBackupSearch(const QString &reason, bool noResults = false);
    void parsePrimaryResults(const QJsonDocument &document);
    void parseBackupCandidates(const QJsonDocument &document);
    void parseBackupLyrics(const QJsonDocument &document);
    void fetchNextBackupLyrics();
    void finishBackupSearch();
    void showSearchResults(const QString &source);
    void updateSelection();
    void clearOverwriteConfirmation();
    void saveSelectedLyrics(bool overwriteConfirmed);
    void setStatus(const QString &text, StatusTone tone = StatusTone::Neutral);

    /** 固定为打开弹框时的音频路径，后续切歌不改变下载目标。 */
    QString m_audioPath;
    /** 本地歌曲时长，单位为毫秒；0 表示无法读取。 */
    qint64 m_audioDurationMs = 0;
    QNetworkAccessManager *m_networkManager;
    QPointer<QNetworkReply> m_reply;
    quint64 m_searchGeneration = 0;
    SearchStage m_searchStage = SearchStage::PrimarySearch;
    QElapsedTimer m_sourceElapsed;
    QString m_searchKeyword;
    QString m_primaryFailure;
    QString m_backupFailure;
    bool m_primaryNoResults = false;
    bool m_backupHadError = false;
    QList<BackupCandidate> m_backupCandidates;
    int m_backupCandidateIndex = 0;
    QList<LyricResult> m_results;
    int m_pendingOverwriteRow = -1;
    QComboBox *m_sourceSelector = nullptr;
    Source m_searchSource = Source::Automatic;
    bool m_saving = false;
    QLineEdit *m_searchEditor = nullptr;
    QPushButton *m_searchButton = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_overwriteLabel = nullptr;
    QTableWidget *m_resultTable = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QWidget *m_overwritePanel = nullptr;
    QPushButton *m_downloadButton = nullptr;
    QPushButton *m_closeButton = nullptr;
};

#endif // LYRICS_DOWNLOAD_DIALOG_H
