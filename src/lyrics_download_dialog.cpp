#include "lyrics_download_dialog.h"

#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringConverter>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <audioproperties.h>
#include <cmath>
#include <fileref.h>
#include <limits>
#include <utility>

namespace {
constexpr int SourceTimeoutMs = 20000;
constexpr int BackupLyricsTimeoutMs = 5000;
constexpr int MaximumBackupCandidates = 5;

QString requestFailure(QNetworkReply *reply)
{
    if (reply->property("lyricsRequestTimedOut").toBool() ||
        reply->error() == QNetworkReply::TimeoutError) {
        return QStringLiteral("请求超时");
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 429) {
        return QStringLiteral("请求过于频繁，服务暂时限流");
    }
    if (status >= 500) {
        return QStringLiteral("服务暂时不可用");
    }
    if (status > 0 && (status < 200 || status >= 300)) {
        return QStringLiteral("服务拒绝请求（HTTP %1）").arg(status);
    }
    if (reply->error() != QNetworkReply::NoError) {
        return QStringLiteral("网络连接失败");
    }
    if (status == 0) {
        return QStringLiteral("服务未返回有效响应");
    }
    return {};
}

bool containsLyricText(const QString &lyrics)
{
    static const QRegularExpression tags(QStringLiteral("^(?:\\[[^\\]\\r\\n]+\\])+"));
    for (QString line : lyrics.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        line.remove(tags);
        if (!line.trimmed().isEmpty() && !line.startsWith(QLatin1Char('<'))) {
            return true;
        }
    }
    return false;
}

class LyricsResultTable final : public QTableWidget
{
public:
    explicit LyricsResultTable(QWidget *parent) : QTableWidget(parent)
    {
        setColumnCount(5);
        setHorizontalHeaderLabels({QStringLiteral("歌名"), QStringLiteral("歌手"),
                                   QStringLiteral("专辑"), QStringLiteral("时长"),
                                   QStringLiteral("滚动歌词")});
        horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        horizontalHeader()->setMinimumSectionSize(0);
        horizontalHeader()->setStretchLastSection(false);
        horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        horizontalHeaderItem(3)->setTextAlignment(Qt::AlignCenter);
        horizontalHeaderItem(4)->setTextAlignment(Qt::AlignCenter);
        verticalHeader()->setVisible(false);
        verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        verticalHeader()->setDefaultSectionSize(qMax(36, fontMetrics().height() + 16));
        setSelectionBehavior(QAbstractItemView::SelectRows);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setSortingEnabled(false);
        setShowGrid(false);
        setWordWrap(false);
        setTextElideMode(Qt::ElideRight);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setMouseTracking(true);
        connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] {
            // 等滚动条占用宽度更新后再分配列宽，避免末列被裁剪。
            QTimer::singleShot(0, this, [this] {
                updateColumnWidths();
            });
        });
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QTableWidget::resizeEvent(event);
        updateColumnWidths();
    }

private:
    void updateColumnWidths()
    {
        constexpr int durationWidth = 70;
        const int scrollingWidth = qMax(
            90,
            horizontalHeader()->fontMetrics().horizontalAdvance(QStringLiteral("滚动歌词")) + 24);
        const int available = qMax(0, viewport()->width() - durationWidth - scrollingWidth);
        const int titleWidth = available * 2 / 5;
        const int artistWidth = available / 5;
        setColumnWidth(0, titleWidth);
        setColumnWidth(1, artistWidth);
        setColumnWidth(2, available - titleWidth - artistWidth);
        setColumnWidth(3, durationWidth);
        setColumnWidth(4, scrollingWidth);
    }
};

qint64 audioDurationMs(const QString &audioPath)
{
#ifdef Q_OS_WIN
    TagLib::FileRef file(audioPath.toStdWString().c_str());
#else
    TagLib::FileRef file(QFile::encodeName(audioPath).constData());
#endif
    if (file.isNull() || file.audioProperties() == nullptr) {
        return 0;
    }
    return qMax<qint64>(0, file.audioProperties()->lengthInMilliseconds());
}

QString durationText(qint64 milliseconds)
{
    const qint64 seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
} // namespace

LyricsDownloadDialog::LyricsDownloadDialog(const QString &audioPath,
                                           QNetworkAccessManager *networkManager,
                                           QWidget *backgroundSurface, const QPixmap &backdrop,
                                           QWidget *parent)
    : AppDialog(QStringLiteral("下载歌词"), backgroundSurface, backdrop, parent),
      m_audioPath(audioPath), m_audioDurationMs(audioDurationMs(audioPath)),
      m_networkManager(networkManager)
{
    setObjectName(QStringLiteral("lyricsDownloadDialog"));
    setMinimumSize(600, 480);
    resize(760, 660);
    auto *layout = bodyLayout();
    auto *searchRow = new QHBoxLayout;
    searchRow->setSpacing(10);
    m_searchEditor = new QLineEdit(QFileInfo(audioPath).completeBaseName(), this);
    m_searchEditor->setPlaceholderText(QStringLiteral("输入歌曲名称"));
    m_searchEditor->setAccessibleName(QStringLiteral("歌曲名称"));
    m_searchEditor->setClearButtonEnabled(true);
    m_searchButton = new QPushButton(QStringLiteral("搜索"), this);
    m_searchButton->setObjectName(QStringLiteral("primaryDialogButton"));
    m_searchButton->setAutoDefault(false);
    m_sourceSelector = new QComboBox(this);
    m_sourceSelector->setAccessibleName(QStringLiteral("歌词来源"));
    m_sourceSelector->addItem(QStringLiteral("自动切换"), static_cast<int>(Source::Automatic));
    m_sourceSelector->addItem(QStringLiteral("LRCLIB"), static_cast<int>(Source::Lrclib));
    m_sourceSelector->addItem(QStringLiteral("酷狗"), static_cast<int>(Source::Kugou));
    m_sourceSelector->setToolTip(
        QStringLiteral("自动：先 LRCLIB，失败后尝试酷狗；也可指定单一来源"));
    searchRow->addWidget(m_searchEditor, 1);
    searchRow->addWidget(m_sourceSelector);
    searchRow->addWidget(m_searchButton);
    layout->addLayout(searchRow);

    auto *songInfo = new QLabel(this);
    songInfo->setObjectName(QStringLiteral("dialogDescription"));
    songInfo->setTextFormat(Qt::PlainText);
    songInfo->setWordWrap(true);
    songInfo->setText(m_audioDurationMs > 0
                          ? QStringLiteral("本地歌曲时长：%1").arg(durationText(m_audioDurationMs))
                          : QStringLiteral("无法读取本地歌曲时长。"));
    layout->addWidget(songInfo);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("lyricsStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_resultTable = new LyricsResultTable(this);
    m_resultTable->setObjectName(QStringLiteral("lyricsResultTable"));
    m_resultTable->setMinimumHeight(100);
    layout->addWidget(m_resultTable, 2);
    auto *previewLabel = new QLabel(QStringLiteral("歌词预览"), this);
    layout->addWidget(previewLabel);
    m_preview = new QPlainTextEdit(this);
    m_preview->setObjectName(QStringLiteral("lyricsPreview"));
    m_preview->setReadOnly(true);
    m_preview->setPlaceholderText(QStringLiteral("选择搜索结果后，在此预览歌词"));
    layout->addWidget(m_preview, 3);

    m_overwritePanel = new QWidget(this);
    m_overwritePanel->setObjectName(QStringLiteral("lyricsOverwritePanel"));
    auto *overwriteLayout = new QHBoxLayout(m_overwritePanel);
    overwriteLayout->setContentsMargins(10, 8, 10, 8);
    m_overwriteLabel = new QLabel(m_overwritePanel);
    m_overwriteLabel->setWordWrap(true);
    auto *confirmOverwrite = new QPushButton(QStringLiteral("覆盖"), m_overwritePanel);
    confirmOverwrite->setObjectName(QStringLiteral("primaryDialogButton"));
    confirmOverwrite->setAutoDefault(false);
    auto *cancelOverwrite = new QPushButton(QStringLiteral("保留原歌词"), m_overwritePanel);
    cancelOverwrite->setObjectName(QStringLiteral("secondaryDialogButton"));
    cancelOverwrite->setAutoDefault(false);
    overwriteLayout->addWidget(m_overwriteLabel, 1);
    overwriteLayout->addWidget(confirmOverwrite);
    overwriteLayout->addWidget(cancelOverwrite);
    layout->addWidget(m_overwritePanel);
    m_overwritePanel->hide();

    auto *actions = new QHBoxLayout;
    actions->setSpacing(10);
    actions->addStretch();
    m_downloadButton = new QPushButton(QStringLiteral("下载"), this);
    m_downloadButton->setObjectName(QStringLiteral("primaryDialogButton"));
    m_downloadButton->setAutoDefault(false);
    m_downloadButton->setEnabled(false);
    m_closeButton = new QPushButton(QStringLiteral("取消"), this);
    m_closeButton->setObjectName(QStringLiteral("secondaryDialogButton"));
    m_closeButton->setAutoDefault(false);
    actions->addWidget(m_downloadButton);
    actions->addWidget(m_closeButton);
    layout->addLayout(actions);

    connect(m_sourceSelector, &QComboBox::currentIndexChanged, this,
            &LyricsDownloadDialog::startSearch);
    connect(m_searchButton, &QPushButton::clicked, this, &LyricsDownloadDialog::startSearch);
    connect(m_searchEditor, &QLineEdit::returnPressed, this, &LyricsDownloadDialog::startSearch);
    connect(m_resultTable, &QTableWidget::currentCellChanged, this,
            &LyricsDownloadDialog::updateSelection);
    connect(m_downloadButton, &QPushButton::clicked, this, [this] {
        saveSelectedLyrics(false);
    });
    connect(confirmOverwrite, &QPushButton::clicked, this, [this] {
        saveSelectedLyrics(true);
    });
    connect(cancelOverwrite, &QPushButton::clicked, this, [this] {
        clearOverwriteConfirmation();
        updateSelection();
        setStatus(QStringLiteral("已保留原歌词，可以继续选择或搜索其他版本。"));
    });
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this] {
        cancelSearch();
    });

    m_searchEditor->setFocus();
    // 先显示完整弹框，再开始首轮搜索，网络等待不阻塞窗口打开。
    QTimer::singleShot(0, this, &LyricsDownloadDialog::startSearch);
}

LyricsDownloadDialog::~LyricsDownloadDialog()
{
    cancelSearch();
}

void LyricsDownloadDialog::cancelSearch()
{
    ++m_searchGeneration;
    if (m_reply == nullptr) {
        return;
    }
    QNetworkReply *reply = m_reply.data();
    m_reply.clear();
    // abort 可能立即发出 finished，先断开回调，防止取消的请求改变界面。
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
}

void LyricsDownloadDialog::startSearch()
{
    if (m_saving) {
        return;
    }
    m_searchSource = static_cast<Source>(m_sourceSelector->currentData().toInt());
    cancelSearch();
    clearOverwriteConfirmation();
    m_resultTable->setRowCount(0);
    m_results.clear();
    m_preview->clear();
    m_downloadButton->setEnabled(false);
    m_closeButton->setText(QStringLiteral("取消"));

    const QString keyword = m_searchEditor->text().trimmed();
    if (keyword.isEmpty()) {
        setStatus(QStringLiteral("请输入歌曲名称后搜索。"), StatusTone::Error);
        m_searchEditor->setFocus();
        return;
    }

    m_searchKeyword = keyword;
    m_searchStage = SearchStage::PrimarySearch;
    m_primaryFailure.clear();
    m_backupFailure.clear();
    m_primaryNoResults = false;
    m_backupHadError = false;
    m_backupCandidates.clear();
    m_backupCandidateIndex = 0;
    m_sourceElapsed.start();
    if (m_searchSource == Source::Kugou) {
        startBackupSearch({});
        return;
    }
    QUrl url(QStringLiteral("https://lrclib.net/api/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), m_searchKeyword);
    url.setQuery(query);
    setStatus(QStringLiteral("正在通过 LRCLIB 搜索“%1”……").arg(m_searchKeyword));
    sendSearchRequest(url);
}

void LyricsDownloadDialog::sendSearchRequest(const QUrl &url)
{
    const qint64 remaining = SourceTimeoutMs - m_sourceElapsed.elapsed();
    if (remaining <= 0) {
        handleSearchFailure(QStringLiteral("请求超时"));
        return;
    }
    const int timeout = m_searchStage == SearchStage::BackupLyrics
                            ? static_cast<int>(qMin<qint64>(BackupLyricsTimeoutMs, remaining))
                            : static_cast<int>(remaining);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("WhqMusic/0.1 (local music player)"));
    request.setTransferTimeout(timeout);
    QNetworkReply *reply = m_networkManager->get(request);
    m_reply = reply;
    const quint64 generation = m_searchGeneration;
    // 传输超时不能限制持续少量返回的数据，另设总时限防止一直等待。
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, [reply] {
        reply->setProperty("lyricsRequestTimedOut", true);
        reply->abort();
    });
    connect(reply, &QNetworkReply::finished, deadline, &QTimer::stop);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        finishSearch(reply, generation);
    });
    deadline->start(timeout);
}

void LyricsDownloadDialog::finishSearch(QNetworkReply *reply, quint64 generation)
{
    if (generation != m_searchGeneration || m_reply.data() != reply) {
        reply->deleteLater();
        return;
    }
    m_reply.clear();
    const QString failure = requestFailure(reply);
    const QByteArray response = reply->readAll();
    reply->deleteLater();
    if (!failure.isEmpty()) {
        handleSearchFailure(failure);
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        handleSearchFailure(QStringLiteral("返回数据无法识别"));
        return;
    }
    switch (m_searchStage) {
    case SearchStage::PrimarySearch:
        parsePrimaryResults(document);
        break;
    case SearchStage::BackupSearch:
        parseBackupCandidates(document);
        break;
    case SearchStage::BackupLyrics:
        parseBackupLyrics(document);
        break;
    }
}

void LyricsDownloadDialog::handleSearchFailure(const QString &reason)
{
    if (m_searchStage == SearchStage::PrimarySearch) {
        startBackupSearch(reason);
        return;
    }
    m_backupFailure = reason;
    m_backupHadError = true;
    if (m_searchStage == SearchStage::BackupLyrics) {
        fetchNextBackupLyrics();
    } else {
        finishBackupSearch();
    }
}

void LyricsDownloadDialog::startBackupSearch(const QString &reason, bool noResults)
{
    if (m_searchSource == Source::Lrclib) {
        setStatus(QStringLiteral("LRCLIB：%1，请调整歌名或切换歌词来源后重试。").arg(reason),
                  noResults ? StatusTone::Neutral : StatusTone::Error);
        return;
    }
    m_primaryFailure = reason;
    m_primaryNoResults = noResults;
    m_searchStage = SearchStage::BackupSearch;
    m_sourceElapsed.restart();
    setStatus(m_searchSource == Source::Kugou
                  ? QStringLiteral("正在通过酷狗搜索“%1”……").arg(m_searchKeyword)
                  : QStringLiteral("LRCLIB %1，正在尝试备用歌词源：酷狗……").arg(reason));
    QUrl url(QStringLiteral("https://lyrics.kugou.com/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ver"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("man"), QStringLiteral("yes"));
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("pc"));
    query.addQueryItem(QStringLiteral("keyword"), m_searchKeyword);
    query.addQueryItem(QStringLiteral("duration"), QString::number(m_audioDurationMs));
    url.setQuery(query);
    sendSearchRequest(url);
}

void LyricsDownloadDialog::parsePrimaryResults(const QJsonDocument &document)
{
    if (!document.isArray()) {
        handleSearchFailure(QStringLiteral("返回数据格式异常"));
        return;
    }
    for (const QJsonValue &value : document.array()) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        LyricResult result;
        result.lyrics = object.value(QStringLiteral("syncedLyrics")).toString();
        result.synchronized = !result.lyrics.trimmed().isEmpty();
        if (!result.synchronized) {
            result.lyrics = object.value(QStringLiteral("plainLyrics")).toString();
        }
        if (!containsLyricText(result.lyrics)) {
            continue;
        }
        result.title = object.value(QStringLiteral("trackName")).toString().trimmed();
        result.artist = object.value(QStringLiteral("artistName")).toString().trimmed();
        result.album = object.value(QStringLiteral("albumName")).toString().trimmed();
        if (result.title.isEmpty()) {
            result.title = QStringLiteral("未知歌曲");
        }
        if (result.artist.isEmpty()) {
            result.artist = QStringLiteral("未知歌手");
        }
        if (result.album.isEmpty()) {
            result.album = QStringLiteral("未知专辑");
        }
        const QJsonValue duration = object.value(QStringLiteral("duration"));
        const double seconds = duration.toDouble(-1.0);
        if (duration.isDouble() && std::isfinite(seconds) && seconds > 0.0 &&
            seconds < static_cast<double>(std::numeric_limits<qint64>::max() / 1000)) {
            result.durationSeconds = seconds;
        }
        m_results.append(std::move(result));
    }

    if (m_results.isEmpty()) {
        startBackupSearch(QStringLiteral("未找到可用歌词"), true);
        return;
    }
    showSearchResults(QStringLiteral("LRCLIB"));
}

void LyricsDownloadDialog::parseBackupCandidates(const QJsonDocument &document)
{
    const QJsonObject object = document.object();
    if (!document.isObject() || object.value(QStringLiteral("status")).toInt() != 200 ||
        !object.value(QStringLiteral("candidates")).isArray()) {
        handleSearchFailure(QStringLiteral("搜索返回数据异常"));
        return;
    }
    const QJsonArray candidates = object.value(QStringLiteral("candidates")).toArray();
    for (const QJsonValue &value : candidates) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject candidate = value.toObject();
        BackupCandidate item;
        const QJsonValue id = candidate.value(QStringLiteral("id"));
        item.id = id.isString() ? id.toString() : QString::number(id.toInteger(-1));
        item.accessKey = candidate.value(QStringLiteral("accesskey")).toString();
        if (item.id.isEmpty() || item.id == QStringLiteral("-1") || item.accessKey.isEmpty()) {
            continue;
        }
        item.result.title = candidate.value(QStringLiteral("song")).toString().trimmed();
        item.result.artist = candidate.value(QStringLiteral("singer")).toString().trimmed();
        item.result.album = QStringLiteral("未知专辑");
        if (item.result.title.isEmpty()) {
            continue;
        }
        if (item.result.artist.isEmpty()) {
            item.result.artist = QStringLiteral("未知歌手");
        }
        const double duration = candidate.value(QStringLiteral("duration")).toDouble(-1.0);
        if (std::isfinite(duration) && duration > 0.0 &&
            duration < static_cast<double>(std::numeric_limits<qint64>::max())) {
            item.result.durationSeconds = duration / 1000.0;
        }
        m_backupCandidates.append(std::move(item));
    }
    if (m_backupCandidates.isEmpty()) {
        if (!candidates.isEmpty()) {
            m_backupHadError = true;
            m_backupFailure = QStringLiteral("搜索结果缺少必要信息");
        }
        finishBackupSearch();
        return;
    }
    if (m_audioDurationMs > 0) {
        const double localSeconds = m_audioDurationMs / 1000.0;
        std::stable_sort(m_backupCandidates.begin(), m_backupCandidates.end(),
                         [localSeconds](const BackupCandidate &left, const BackupCandidate &right) {
                             const double leftDifference =
                                 left.result.durationSeconds > 0.0
                                     ? std::abs(left.result.durationSeconds - localSeconds)
                                     : std::numeric_limits<double>::infinity();
                             const double rightDifference =
                                 right.result.durationSeconds > 0.0
                                     ? std::abs(right.result.durationSeconds - localSeconds)
                                     : std::numeric_limits<double>::infinity();
                             return leftDifference < rightDifference;
                         });
    }
    // 控制请求数量，并共享备用源总时限，避免候选过多导致长时间等待。
    if (m_backupCandidates.size() > MaximumBackupCandidates) {
        m_backupCandidates.resize(MaximumBackupCandidates);
    }
    fetchNextBackupLyrics();
}

void LyricsDownloadDialog::fetchNextBackupLyrics()
{
    if (m_sourceElapsed.elapsed() >= SourceTimeoutMs) {
        m_backupHadError = true;
        m_backupFailure = QStringLiteral("请求超时");
        finishBackupSearch();
        return;
    }
    if (m_backupCandidateIndex >= m_backupCandidates.size()) {
        finishBackupSearch();
        return;
    }
    m_searchStage = SearchStage::BackupLyrics;
    const BackupCandidate &candidate = m_backupCandidates.at(m_backupCandidateIndex++);
    setStatus(QStringLiteral("正在从酷狗获取歌词：%1 / %2……")
                  .arg(m_backupCandidateIndex)
                  .arg(m_backupCandidates.size()));
    QUrl url(QStringLiteral("https://lyrics.kugou.com/download"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ver"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("pc"));
    query.addQueryItem(QStringLiteral("id"), candidate.id);
    query.addQueryItem(QStringLiteral("accesskey"), candidate.accessKey);
    query.addQueryItem(QStringLiteral("fmt"), QStringLiteral("lrc"));
    query.addQueryItem(QStringLiteral("charset"), QStringLiteral("utf8"));
    url.setQuery(query);
    sendSearchRequest(url);
}

void LyricsDownloadDialog::parseBackupLyrics(const QJsonDocument &document)
{
    const QJsonObject object = document.object();
    if (!document.isObject() || object.value(QStringLiteral("status")).toInt() != 200 ||
        !object.value(QStringLiteral("content")).isString()) {
        handleSearchFailure(QStringLiteral("歌词返回数据异常"));
        return;
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        object.value(QStringLiteral("content")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok) {
        handleSearchFailure(QStringLiteral("歌词内容无法解码"));
        return;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString lyrics = decoder(decoded.decoded);
    if (decoder.hasError()) {
        handleSearchFailure(QStringLiteral("歌词文本编码异常"));
        return;
    }
    if (containsLyricText(lyrics)) {
        LyricResult result = m_backupCandidates.at(m_backupCandidateIndex - 1).result;
        result.lyrics = lyrics;
        static const QRegularExpression timestamp(
            QStringLiteral("\\[\\d{1,3}:\\d{2}(?:[.:]\\d{1,3})?\\]"));
        result.synchronized = timestamp.match(lyrics).hasMatch();
        m_results.append(std::move(result));
    }
    fetchNextBackupLyrics();
}

void LyricsDownloadDialog::finishBackupSearch()
{
    if (!m_results.isEmpty()) {
        showSearchResults(QStringLiteral("酷狗"));
        return;
    }
    if (m_searchSource == Source::Kugou) {
        setStatus(QStringLiteral("酷狗：%1，请调整歌名或切换歌词来源后重试。")
                      .arg(m_backupHadError ? m_backupFailure : QStringLiteral("未找到可用歌词")),
                  m_backupHadError ? StatusTone::Error : StatusTone::Neutral);
        return;
    }
    if (m_primaryNoResults && !m_backupHadError) {
        setStatus(QStringLiteral("LRCLIB 和酷狗均未找到可用歌词，请调整歌曲名称后重新搜索。"));
        return;
    }
    const QString backupReason =
        m_backupHadError ? m_backupFailure : QStringLiteral("未找到可用歌词");
    setStatus(QStringLiteral(
                  "两个歌词源均未取得可用歌词。LRCLIB：%1；酷狗：%2。请稍后重试或调整歌曲名称。")
                  .arg(m_primaryFailure, backupReason),
              StatusTone::Error);
}

void LyricsDownloadDialog::showSearchResults(const QString &source)
{
    const double localSeconds = m_audioDurationMs / 1000.0;
    const auto difference = [localSeconds](const LyricResult &result) {
        return result.durationSeconds > 0.0 ? std::abs(result.durationSeconds - localSeconds)
                                            : std::numeric_limits<double>::infinity();
    };
    // 同步歌词优先；组内按时长差排序，无法比较或差值相同时保留接口顺序。
    std::stable_sort(
        m_results.begin(), m_results.end(),
        [localSeconds, &difference](const LyricResult &left, const LyricResult &right) {
            if (left.synchronized != right.synchronized) {
                return left.synchronized;
            }
            if (localSeconds <= 0.0) {
                return false;
            }
            return difference(left) < difference(right);
        });

    {
        const QSignalBlocker blocker(m_resultTable);
        m_resultTable->setRowCount(static_cast<int>(m_results.size()));
        for (int row = 0; row < m_results.size(); ++row) {
            const LyricResult &result = m_results.at(row);
            const QString time = result.durationSeconds > 0.0
                                     ? durationText(qRound64(result.durationSeconds * 1000.0))
                                     : QStringLiteral("--:--");
            const QStringList values = {result.title, result.artist, result.album, time,
                                        result.synchronized ? QStringLiteral("支持")
                                                            : QStringLiteral("不支持")};
            for (int column = 0; column < values.size(); ++column) {
                const QString &value = values.at(column);
                auto *item = new QTableWidgetItem(value);
                item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(value.toHtmlEscaped()));
                item->setTextAlignment(column >= 3 ? Qt::AlignCenter
                                                   : Qt::AlignLeft | Qt::AlignVCenter);
                m_resultTable->setItem(row, column, item);
            }
        }
    }
    m_resultTable->setCurrentCell(0, 0);
    updateSelection();
    setStatus(QStringLiteral("来源：%1，找到 %2 个结果，选择版本预览后点击下载。")
                  .arg(source)
                  .arg(m_results.size()));
}

void LyricsDownloadDialog::clearOverwriteConfirmation()
{
    m_pendingOverwriteRow = -1;
    m_overwritePanel->hide();
}

void LyricsDownloadDialog::updateSelection()
{
    clearOverwriteConfirmation();
    const int row = m_resultTable->currentRow();
    const bool valid = m_reply == nullptr && row >= 0 && row < m_results.size();
    m_downloadButton->setEnabled(valid);
    if (!valid) {
        m_preview->clear();
        return;
    }
    m_preview->setPlainText(m_results.at(row).lyrics);
}

void LyricsDownloadDialog::saveSelectedLyrics(bool overwriteConfirmed)
{
    const int row = m_resultTable->currentRow();
    if (m_saving || m_reply != nullptr || row < 0 || row >= m_results.size()) {
        return;
    }
    if (overwriteConfirmed && m_pendingOverwriteRow != row) {
        return;
    }
    const QFileInfo audioInfo(m_audioPath);
    if (!audioInfo.isFile()) {
        clearOverwriteConfirmation();
        setStatus(QStringLiteral("原音乐文件已不存在，无法保存歌词。"), StatusTone::Error);
        return;
    }
    m_saving = true;
    m_searchEditor->setEnabled(false);
    m_searchButton->setEnabled(false);
    m_sourceSelector->setEnabled(false);
    m_resultTable->setEnabled(false);
    m_overwritePanel->setEnabled(false);
    m_closeButton->setEnabled(false);
    m_downloadButton->setEnabled(false);
    setStatus(QStringLiteral("正在保存歌词，请稍候…"));
    emit embeddedSaveRequested(m_audioPath, m_results.at(row).lyrics, overwriteConfirmed);
}

void LyricsDownloadDialog::finishEmbeddedSave(bool success, bool needsOverwrite,
                                              const QString &message)
{
    m_saving = false;
    m_searchEditor->setEnabled(true);
    m_searchButton->setEnabled(true);
    m_sourceSelector->setEnabled(true);
    m_resultTable->setEnabled(true);
    m_overwritePanel->setEnabled(true);
    m_closeButton->setEnabled(true);
    if (needsOverwrite) {
        m_pendingOverwriteRow = m_resultTable->currentRow();
        m_overwriteLabel->setText(QStringLiteral("歌曲已有内嵌歌词，是否覆盖？"));
        m_overwritePanel->show();
        setStatus(QStringLiteral("请选择是否覆盖原歌词。"));
        m_downloadButton->setEnabled(false);
        return;
    }
    clearOverwriteConfirmation();
    m_downloadButton->setEnabled(!success);
    setStatus(message, success ? StatusTone::Success : StatusTone::Error);
    if (success) {
        m_closeButton->setText(QStringLiteral("完成"));
        emit lyricsSaved(m_audioPath);
    }
}

void LyricsDownloadDialog::reject()
{
    if (!m_saving) {
        AppDialog::reject();
    }
}

void LyricsDownloadDialog::setStatus(const QString &text, StatusTone tone)
{
    QString toneName = QStringLiteral("neutral");
    if (tone == StatusTone::Error) {
        toneName = QStringLiteral("error");
    } else if (tone == StatusTone::Success) {
        toneName = QStringLiteral("success");
    }
    m_status->setText(text);
    m_status->setProperty("tone", toneName);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->update();
}
