#include "lyrics_download_dialog.h"

#include <QComboBox>
#include <QCursor>
#include <QDebug>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringConverter>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
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
constexpr int KugouKrcTimeoutMs = 5000;
constexpr int MaximumKugouCandidates = 5;

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

class LyricsResultItemDelegate final : public QStyledItemDelegate
{
public:
    explicit LyricsResultItemDelegate(QTableWidget *table)
        : QStyledItemDelegate(table)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem rowOption(option);
        rowOption.state &= ~QStyle::State_MouseOver;
        rowOption.state &= ~QStyle::State_Selected;
        rowOption.state &= ~QStyle::State_HasFocus;
        QStyledItemDelegate::paint(painter, rowOption, index);
    }
};

class LyricsResultTable final : public QTableWidget
{
public:
    explicit LyricsResultTable(QWidget *parent) : QTableWidget(parent)
    {
        setColumnCount(5);
        setHorizontalHeaderLabels({QStringLiteral("歌名"), QStringLiteral("歌手"),
                                   QStringLiteral("专辑"), QStringLiteral("时长"),
                                   QStringLiteral("滚动模式")});
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
        // 点击未完整显示的行时保持当前位置，避免选中操作触发自动滚动。
        setAutoScroll(false);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setSortingEnabled(false);
        setShowGrid(false);
        setWordWrap(false);
        setTextElideMode(Qt::ElideRight);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setMouseTracking(true);
        setItemDelegate(new LyricsResultItemDelegate(this));
        connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] {
            // 等滚动条占用宽度更新后再分配列宽，避免末列被裁剪。
            QTimer::singleShot(0, this, [this] {
                updateColumnWidths();
            });
        });
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QTableWidget::paintEvent(event);
        QPainter painter(viewport());
        // 与音乐库一致，背景按整行绘制，避免单元格状态叠加造成局部高亮。
        const auto fillRow = [this, &painter](int row, const QColor &color) {
            QRect area = visualRect(model()->index(row, 0));
            if (!area.isValid()) {
                return;
            }
            area.setLeft(0);
            area.setRight(viewport()->width() - 1);
            painter.fillRect(area, color);
        };
        const QModelIndexList selectedRows = selectionModel()->selectedRows();
        for (const QModelIndex &index : selectedRows) {
            fillRow(index.row(), QColor(255, 255, 255, 26));
        }
        // 根据当前鼠标位置计算，滚动后也不会保留旧行的悬停状态。
        const QPoint position = viewport()->mapFromGlobal(QCursor::pos());
        if (!viewport()->underMouse() || !viewport()->rect().contains(position)) {
            return;
        }
        const QModelIndex hovered = indexAt(position);
        if (hovered.isValid() &&
            !selectionModel()->isRowSelected(hovered.row(), QModelIndex())) {
            fillRow(hovered.row(), QColor(255, 255, 255, 16));
        }
    }

    bool viewportEvent(QEvent *event) override
    {
        if (event->type() == QEvent::MouseMove || event->type() == QEvent::Enter ||
            event->type() == QEvent::Leave) {
            // 默认只重绘鼠标经过的单元格，这里让整行各列同步更新高亮。
            viewport()->update();
        }
        return QTableWidget::viewportEvent(event);
    }

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
            horizontalHeader()->fontMetrics().horizontalAdvance(QStringLiteral("滚动模式")) + 24);
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
    m_sourceSelector->addItem(QStringLiteral("酷狗"), static_cast<int>(Source::Kugou));
    m_sourceSelector->addItem(QStringLiteral("LRCLIB"), static_cast<int>(Source::Lrclib));
    m_sourceSelector->setToolTip(
        QStringLiteral("自动：先酷狗，失败或无可用歌词时尝试 LRCLIB；也可指定单一来源"));
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
    m_kugouFailure.clear();
    m_kugouHadError = false;
    m_kugouCandidates.clear();
    m_kugouCandidateIndex = 0;
    if (m_searchSource == Source::Lrclib) {
        startLrclibSearch();
    } else {
        startKugouSearch();
    }
}

void LyricsDownloadDialog::sendSearchRequest(const QUrl &url)
{
    const qint64 remaining = SourceTimeoutMs - m_sourceElapsed.elapsed();
    if (remaining <= 0) {
        handleSearchFailure(QStringLiteral("请求超时"));
        return;
    }
    const int timeout = (m_searchStage == SearchStage::KugouKrc ||
                         m_searchStage == SearchStage::KugouLrc)
                            ? static_cast<int>(qMin<qint64>(KugouKrcTimeoutMs, remaining))
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
    case SearchStage::LrclibSearch:
        parseLrclibResults(document);
        break;
    case SearchStage::KugouSearch:
        parseKugouCandidates(document);
        break;
    case SearchStage::KugouKrc:
    case SearchStage::KugouLrc:
        parseKugouLyrics(document);
        break;
    }
}

void LyricsDownloadDialog::handleSearchFailure(const QString &reason)
{
    qWarning() << "Lyrics request failed, stage:" << static_cast<int>(m_searchStage)
               << "reason:" << reason;
    if (m_searchStage == SearchStage::LrclibSearch) {
        finishLrclibFailure(reason);
        return;
    }
    if (m_searchStage == SearchStage::KugouKrc) {
        // 同一候选先取逐字，失败后只回退一次 LRC，不让失败候选阻止其他结果。
        fetchKugouLyrics(false);
        return;
    }
    m_kugouFailure = reason;
    m_kugouHadError = true;
    if (m_searchStage == SearchStage::KugouLrc) {
        fetchNextKugouLyrics();
    } else {
        finishKugouSearch();
    }
}

void LyricsDownloadDialog::startKugouSearch()
{
    m_searchStage = SearchStage::KugouSearch;
    m_sourceElapsed.restart();
    setStatus(QStringLiteral("正在通过酷狗搜索“%1”……").arg(m_searchKeyword));
    QUrl url(QStringLiteral("https://lyrics.kugou.com/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ver"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("man"), QStringLiteral("yes"));
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("pc"));
    query.addQueryItem(QStringLiteral("keyword"), m_searchKeyword);
    if (m_audioDurationMs > 0) {
        query.addQueryItem(QStringLiteral("duration"), QString::number(m_audioDurationMs));
    }
    url.setQuery(query);
    sendSearchRequest(url);
}

void LyricsDownloadDialog::startLrclibSearch()
{
    m_searchStage = SearchStage::LrclibSearch;
    m_sourceElapsed.restart();
    setStatus(m_searchSource == Source::Automatic
                  ? QStringLiteral("酷狗未取得可用歌词，正在尝试 LRCLIB……")
                  : QStringLiteral("正在通过 LRCLIB 搜索“%1”……").arg(m_searchKeyword));
    QUrl url(QStringLiteral("https://lrclib.net/api/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), m_searchKeyword);
    url.setQuery(query);
    sendSearchRequest(url);
}

void LyricsDownloadDialog::finishLrclibFailure(const QString &reason, bool noResults)
{
    if (m_searchSource == Source::Lrclib) {
        setStatus(QStringLiteral("LRCLIB：%1，请调整歌名或切换来源后重试。").arg(reason),
                  noResults ? StatusTone::Neutral : StatusTone::Error);
        return;
    }
    if (noResults && !m_kugouHadError) {
        setStatus(QStringLiteral("酷狗和 LRCLIB 均未找到可用歌词，请调整歌曲名称后重新搜索。"));
        return;
    }
    const QString kugouReason = m_kugouHadError ? m_kugouFailure : QStringLiteral("未找到可用歌词");
    setStatus(QStringLiteral("两个歌词源均未取得可用歌词。酷狗：%1；LRCLIB：%2。请稍后重试。")
                  .arg(kugouReason, reason), StatusTone::Error);
}

void LyricsDownloadDialog::parseLrclibResults(const QJsonDocument &document)
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
        if (result.lyrics.trimmed().isEmpty()) {
            result.lyrics = object.value(QStringLiteral("plainLyrics")).toString();
        }
        if (!containsLyricText(result.lyrics)) {
            continue;
        }
        result.mode = Lyrics::mode(Lyrics::parseTimed(result.lyrics));
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
        finishLrclibFailure(QStringLiteral("未找到可用歌词"), true);
        return;
    }
    showSearchResults(QStringLiteral("LRCLIB"));
}

void LyricsDownloadDialog::parseKugouCandidates(const QJsonDocument &document)
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
        KugouCandidate item;
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
        m_kugouCandidates.append(std::move(item));
    }
    if (m_kugouCandidates.isEmpty()) {
        if (!candidates.isEmpty()) {
            m_kugouHadError = true;
            m_kugouFailure = QStringLiteral("搜索结果缺少必要信息");
        }
        finishKugouSearch();
        return;
    }
    if (m_audioDurationMs > 0) {
        const double localSeconds = m_audioDurationMs / 1000.0;
        std::stable_sort(m_kugouCandidates.begin(), m_kugouCandidates.end(),
                         [localSeconds](const KugouCandidate &left, const KugouCandidate &right) {
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
    // 控制请求数量，KRC 与 LRC 回退共享酷狗总时限，避免候选过多导致长时间等待。
    if (m_kugouCandidates.size() > MaximumKugouCandidates) {
        m_kugouCandidates.resize(MaximumKugouCandidates);
    }
    fetchNextKugouLyrics();
}

void LyricsDownloadDialog::fetchNextKugouLyrics()
{
    if (m_kugouCandidateIndex >= m_kugouCandidates.size()) {
        finishKugouSearch();
        return;
    }
    ++m_kugouCandidateIndex;
    fetchKugouLyrics(true);
}

void LyricsDownloadDialog::fetchKugouLyrics(bool wordTimed)
{
    if (m_sourceElapsed.elapsed() >= SourceTimeoutMs) {
        m_kugouHadError = true;
        m_kugouFailure = QStringLiteral("请求超时");
        finishKugouSearch();
        return;
    }
    if (m_kugouCandidateIndex <= 0 || m_kugouCandidateIndex > m_kugouCandidates.size()) {
        finishKugouSearch();
        return;
    }
    m_searchStage = wordTimed ? SearchStage::KugouKrc : SearchStage::KugouLrc;
    const KugouCandidate &candidate = m_kugouCandidates.at(m_kugouCandidateIndex - 1);
    setStatus(QStringLiteral("正在从酷狗获取%1歌词：%2 / %3……")
                  .arg(wordTimed ? QStringLiteral("逐字") : QStringLiteral("逐句"))
                  .arg(m_kugouCandidateIndex)
                  .arg(m_kugouCandidates.size()));
    QUrl url(QStringLiteral("https://lyrics.kugou.com/download"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ver"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("pc"));
    query.addQueryItem(QStringLiteral("id"), candidate.id);
    query.addQueryItem(QStringLiteral("accesskey"), candidate.accessKey);
    query.addQueryItem(QStringLiteral("fmt"), wordTimed ? QStringLiteral("krc") : QStringLiteral("lrc"));
    query.addQueryItem(QStringLiteral("charset"), QStringLiteral("utf8"));
    url.setQuery(query);
    sendSearchRequest(url);
}

void LyricsDownloadDialog::parseKugouLyrics(const QJsonDocument &document)
{
    const QJsonObject object = document.object();
    if (!document.isObject() || object.value(QStringLiteral("status")).toInt() != 200 ||
        !object.value(QStringLiteral("content")).isString()) {
        handleSearchFailure(QStringLiteral("歌词返回数据异常"));
        return;
    }
    const QString encoded = object.value(QStringLiteral("content")).toString();
    if (encoded.size() > 4 * 1024 * 1024) {
        handleSearchFailure(QStringLiteral("歌词文件过大"));
        return;
    }
    const auto decoded = QByteArray::fromBase64Encoding(encoded.toLatin1(),
                                                        QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok) {
        handleSearchFailure(QStringLiteral("歌词内容无法解码"));
        return;
    }
    QString lyrics;
    if (m_searchStage == SearchStage::KugouKrc) {
        QString error;
        if (!Lyrics::decodeKrc(decoded.decoded, lyrics, error)) {
            handleSearchFailure(error);
            return;
        }
    } else {
        QStringDecoder decoder(QStringDecoder::Utf8);
        lyrics = decoder(decoded.decoded);
        if (decoder.hasError()) {
            handleSearchFailure(QStringLiteral("歌词文本编码异常"));
            return;
        }
    }
    if (!containsLyricText(Lyrics::previewText(lyrics))) {
        handleSearchFailure(QStringLiteral("歌词内容为空或无效"));
        return;
    }
    LyricResult result = m_kugouCandidates.at(m_kugouCandidateIndex - 1).result;
    // 保留解码后的完整 KRC 文本写入原有歌词标签，重新载入时仍可恢复字词时间。
    result.lyrics = lyrics;
    result.mode = Lyrics::mode(Lyrics::parseTimed(lyrics));
    m_results.append(std::move(result));
    fetchNextKugouLyrics();
}

void LyricsDownloadDialog::finishKugouSearch()
{
    if (!m_results.isEmpty()) {
        showSearchResults(QStringLiteral("酷狗"));
        return;
    }
    if (m_searchSource == Source::Automatic) {
        startLrclibSearch();
        return;
    }
    setStatus(QStringLiteral("酷狗：%1，请调整歌名或切换歌词来源后重试。")
                  .arg(m_kugouHadError ? m_kugouFailure : QStringLiteral("未找到可用歌词")),
              m_kugouHadError ? StatusTone::Error : StatusTone::Neutral);
}

void LyricsDownloadDialog::showSearchResults(const QString &source)
{
    const double localSeconds = m_audioDurationMs / 1000.0;
    const auto difference = [localSeconds](const LyricResult &result) {
        return result.durationSeconds > 0.0 ? std::abs(result.durationSeconds - localSeconds)
                                            : std::numeric_limits<double>::infinity();
    };
    // 最终结果按逐字、逐句、无排序；同模式内按时长差排序，相同则保留接口顺序。
    std::stable_sort(
        m_results.begin(), m_results.end(),
        [localSeconds, &difference](const LyricResult &left, const LyricResult &right) {
            if (left.mode != right.mode) {
                return static_cast<int>(left.mode) > static_cast<int>(right.mode);
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
                                        Lyrics::modeText(result.mode)};
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
    m_preview->setPlainText(Lyrics::previewText(m_results.at(row).lyrics));
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
