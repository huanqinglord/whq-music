#include "main_window.h"
#include "player_widgets.h"
#include "music_library_repository.h"
#include "music_library_service.h"
#include "music_file_utils.h"
#include "playback_controller.h"
#include "metadata_scanner.h"

#include <QDir>
#include <QDebug>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVariant>
#include <QtConcurrent>

#include <memory>
#include <utility>

using namespace PlayerWidgets;

bool MainWindow::initializeDatabase()
{
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const bool directoryReady = QDir().mkpath(directory);
    m_database.setDatabaseName(directory + QStringLiteral("/music.db"));
    m_repository = std::make_unique<MusicLibraryRepository>(m_database);
    QString error;
    if (!directoryReady || !m_database.open() || !m_repository->initialize(error)) {
        QMessageBox::critical(this, QStringLiteral("启动失败"),
                              error.isEmpty() ? QStringLiteral("无法打开音乐数据库。") : error);
        return false;
    }
    return true;
}

void MainWindow::loadGroups()
{
    QList<MusicGroup> groups;
    QString error;
    if (!m_repository->loadGroups(groups, error)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), error);
        return;
    }
    const bool hadSelectedGroup = m_groups->currentItem() != nullptr;
    const qint64 selectedGroupId = m_activeGroupId;
    QSignalBlocker groupSelectionBlocker(m_groups);
    m_groupEditor = nullptr;
    m_groups->clear();
    addGroupItem(0, QStringLiteral("全部音乐"));
    for (const MusicGroup &group : groups) {
        addGroupItem(group.id, group.name);
    }
    auto *createItem = new QListWidgetItem(m_groups);
    createItem->setFlags(createItem->flags() & ~Qt::ItemIsSelectable);
    auto *createButton = new QPushButton(QStringLiteral("+ 新建分组"), m_groups);
    createButton->setObjectName(QStringLiteral("newGroupButton"));
    createButton->setFocusPolicy(Qt::NoFocus);
    configureIconButton(createButton, QStringLiteral(":/icons/add_circle.svg"),
                        QStringLiteral("新建分组"));
    createItem->setSizeHint(QSize(0, 42));
    m_groups->setItemWidget(createItem, createButton);
    connect(createButton, &QPushButton::clicked, this, &MainWindow::createGroup);

    QListWidgetItem *itemToSelect = m_groups->item(0);
    bool restoredSelectedGroup = selectedGroupId == 0;
    for (int row = 0; row < m_groups->count() - 1; ++row) {
        QListWidgetItem *item = m_groups->item(row);
        if (item->data(Qt::UserRole).toLongLong() == selectedGroupId) {
            itemToSelect = item;
            restoredSelectedGroup = true;
            break;
        }
    }
    m_groups->setCurrentItem(itemToSelect);
    if (!restoredSelectedGroup) {
        m_activeGroupId = 0;
    }
    groupSelectionBlocker.unblock();
    if (!hadSelectedGroup || !restoredSelectedGroup) {
        loadSongs();
    }
}

void MainWindow::addGroupItem(qint64 groupId, const QString &name)
{
    auto *item = new QListWidgetItem(m_groups);
    item->setData(Qt::UserRole, groupId);
    item->setSizeHint(QSize(0, 42));

    auto *rowWidget = new QWidget(m_groups);
    auto *rowLayout = new QHBoxLayout(rowWidget);
    rowLayout->setContentsMargins(4, 0, 4, 0);
    rowLayout->setSpacing(4);
    auto *nameButton = new QPushButton(name, rowWidget);
    nameButton->setObjectName(QStringLiteral("groupNameButton"));
    auto *menuButton = new QPushButton(QStringLiteral("…"), rowWidget);
    menuButton->setObjectName(QStringLiteral("groupMenuButton"));
    configureIconButton(menuButton, QStringLiteral(":/icons/more_horizontal.svg"),
                        QStringLiteral("分组操作"));
    menuButton->setFixedWidth(34);
    rowLayout->addWidget(nameButton, 1);
    rowLayout->addWidget(menuButton);
    m_groups->setItemWidget(item, rowWidget);

    connect(nameButton, &QPushButton::clicked, this, [this, item] {
        m_groups->setCurrentItem(item);
    });
    connect(menuButton, &QPushButton::clicked, this, [this, item, groupId, name, menuButton] {
        m_groups->setCurrentItem(item);
        showGroupMenu(groupId, name, menuButton);
    });
}

void MainWindow::loadSongs(bool reloadLibrary)
{
    if (m_searchTimer != nullptr) {
        m_searchTimer->stop();
    }
    const int scrollPosition = m_songsTable->verticalScrollBar()->value();
    qint64 selectedId = 0;
    const QModelIndexList selectedRows = m_songsTable->selectionModel()->selectedRows();
    if (!selectedRows.isEmpty()) {
        selectedId = selectedRows.first().data(Qt::UserRole).toLongLong();
    }
    QString error;
    if (reloadLibrary) {
        QList<Song> songs;
        if (!m_repository->loadSongs(songs, error)) {
            QMessageBox::warning(this, QStringLiteral("读取失败"), error);
            return;
        }
        m_librarySongs = songs;
        synchronizePlaybackQueue();
        refreshFileSizeCache();
    }
    QSet<qint64> groupSongs;
    if (currentGroupId() > 0 && !m_repository->groupSongs(currentGroupId(), groupSongs, error)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), error);
        return;
    }
    const QString keyword = m_searchEditor->text().trimmed();
    QList<Song> filteredSongs;
    for (const Song &song : std::as_const(m_librarySongs)) {
        if (currentGroupId() > 0 && !groupSongs.contains(song.id)) {
            continue;
        }
        if (!keyword.isEmpty() && !song.title.contains(keyword, Qt::CaseInsensitive) &&
            !song.artist.contains(keyword, Qt::CaseInsensitive)) {
            continue;
        }
        filteredSongs.append(song);
    }
    m_songsTable->setUpdatesEnabled(false);
    const QSignalBlocker selectionBlocker(m_songsTable->selectionModel());
    m_songModel->setSongs(std::move(filteredSongs));
    for (int row = 0; row < m_songs.size(); ++row) {
        const Song &song = m_songs.at(row);
        if (song.id == selectedId) {
            m_songsTable->selectionModel()->select(m_songsTable->model()->index(row, 0),
                                                   QItemSelectionModel::ClearAndSelect |
                                                       QItemSelectionModel::Rows);
        }
    }
    m_songsTable->verticalScrollBar()->setValue(scrollPosition);
    m_songsTable->setUpdatesEnabled(true);
    updatePlayingIndicators();
    updateTaskbarToolbar();
}

void MainWindow::refreshFileSizeCache()
{
    const int generation = ++m_fileSizeGeneration;
    QStringList paths;
    for (const Song &song : std::as_const(m_librarySongs)) {
        paths.append(song.path);
    }
    auto *watcher = new QFutureWatcher<QHash<QString, qint64>>(this);
    connect(watcher, &QFutureWatcher<QHash<QString, qint64>>::finished, this,
            [this, watcher, generation] {
                if (generation == m_fileSizeGeneration) {
                    m_fileSizeCache = watcher->result();
                    m_songModel->refreshFileSizes();
                }
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run(collectFileSizes, paths));
}

void MainWindow::synchronizePlaybackQueue()
{
    if (!m_playbackQueueRestored) {
        restorePlaybackQueue();
        return;
    }
    m_playbackController->synchronize(m_librarySongs);
}

void MainWindow::restorePlaybackQueue()
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key=?"));
    query.addBindValue(QStringLiteral("playbackQueue"));
    if (!query.exec()) {
        qWarning() << "Failed to load saved playback queue";
        // Keep persistence disabled so a failed read cannot overwrite the saved queue.
        return;
    }
    const QString saved = query.next() ? query.value(0).toString() : QStringLiteral("[]");
    query.finish();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(saved.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        qWarning() << "Saved playback queue has an invalid format";
    }
    QHash<qint64, Song> byId;
    for (const Song &song : std::as_const(m_librarySongs)) {
        byId.insert(song.id, song);
    }
    QList<Song> songs;
    QSet<qint64> restoredIds;
    for (const QJsonValue &value : document.array()) {
        bool valid = false;
        const qint64 id = value.toString().toLongLong(&valid);
        if (valid && byId.contains(id) && !restoredIds.contains(id)) {
            songs.append(byId.value(id));
            restoredIds.insert(id);
        }
    }
    // The signal emitted by restoration must not save the initial queue over history.
    m_savedPlaybackQueue = saved;
    m_playbackController->restoreQueue(songs);
    m_playbackQueueRestored = true;
}

void MainWindow::savePlaybackQueue()
{
    if (!m_playbackQueueRestored) {
        return;
    }
    QJsonArray ids;
    for (const Song &song : m_playbackController->queue()) {
        // Store 64-bit database IDs as strings to preserve their full precision.
        ids.append(QString::number(song.id));
    }
    const QString saved =
        QString::fromUtf8(QJsonDocument(ids).toJson(QJsonDocument::Compact));
    if (saved == m_savedPlaybackQueue) {
        return;
    }
    if (saveSetting(QStringLiteral("playbackQueue"), saved)) {
        m_savedPlaybackQueue = saved;
    }
}

void MainWindow::rebuildPlaybackQueue()
{
    const int scrollPosition = m_queue->verticalScrollBar()->value();
    const qint64 selectedId = m_queue->currentItem() == nullptr
                                  ? 0
                                  : m_queue->currentItem()->data(Qt::UserRole + 1).toLongLong();
    const QSignalBlocker blocker(m_queue);
    m_queue->setUpdatesEnabled(false);
    const QList<Song> &songs = m_playbackController->queue();
    QSet<qint64> retainedIds;
    for (const Song &song : songs) {
        retainedIds.insert(song.id);
    }
    QHash<qint64, QListWidgetItem *> existingItems;
    for (int row = m_queue->count() - 1; row >= 0; --row) {
        QListWidgetItem *item = m_queue->item(row);
        const qint64 id = item->data(Qt::UserRole + 1).toLongLong();
        if (!retainedIds.contains(id)) {
            delete m_queue->takeItem(row);
        } else {
            existingItems.insert(id, item);
        }
    }
    for (int row = 0; row < songs.size(); ++row) {
        const Song &song = songs.at(row);
        QListWidgetItem *item = existingItems.value(song.id, nullptr);
        if (item == nullptr) {
            item = new QListWidgetItem(song.title);
            item->setData(Qt::UserRole, song.path);
            item->setData(Qt::UserRole + 1, song.id);
            m_queue->insertItem(row, item);
        } else {
            if (m_queue->item(row) != item) {
                m_queue->takeItem(m_queue->row(item));
                m_queue->insertItem(row, item);
            }
            if (item->text() != song.title) {
                item->setText(song.title);
            }
            if (item->data(Qt::UserRole).toString() != song.path) {
                item->setData(Qt::UserRole, song.path);
            }
        }
        item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(song.title.toHtmlEscaped()));
        if (song.id == selectedId) {
            m_queue->setCurrentItem(item);
        }
    }
    filterPlaybackQueue();
    m_queue->verticalScrollBar()->setValue(scrollPosition);
    m_queue->setUpdatesEnabled(true);
}

void MainWindow::filterPlaybackQueue()
{
    const QString keyword = m_queueSearchEditor->text().trimmed();
    // 只隐藏不匹配的行，保留列表索引与实际播放队列的一一对应关系。
    for (int row = 0; row < m_queue->count(); ++row) {
        QListWidgetItem *item = m_queue->item(row);
        item->setHidden(!keyword.isEmpty() &&
                        !item->text().contains(keyword, Qt::CaseInsensitive));
    }
}

void MainWindow::updatePlayingIndicators()
{
    m_songsTable->setProperty("playingSongId", m_player->source().isEmpty()
                                                   ? qint64(0)
                                                   : m_playbackController->currentId());
    m_songsTable->setProperty("songIsPlaying",
                              m_player->playbackState() == QMediaPlayer::PlayingState);
    m_songsTable->viewport()->update();
    m_queue->viewport()->update();
}

void MainWindow::refreshMusicLibrary(bool retryFailedScans)
{
    if (m_metadataReadInProgress || m_libraryRefreshInProgress || m_libraryImportInProgress ||
        m_fileEditInProgress) {
        return;
    }
    QList<LibrarySongRecord> songs;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id,title,path,content_sha256 FROM songs ORDER BY id"))) {
        QMessageBox::warning(this, QStringLiteral("刷新失败"),
                             QStringLiteral("无法读取音乐库记录。"));
        return;
    }
    while (query.next()) {
        songs.append({query.value(0).toLongLong(), query.value(1).toString(),
                      query.value(2).toString(), query.value(3).toString()});
    }
    m_libraryRefreshInProgress = true;
    m_refreshLibraryButton->setEnabled(false);
    updateLibraryRefreshProgress(0, 1);
    auto *watcher = new QFutureWatcher<LibraryRefreshResult>(this);
    connect(watcher, &QFutureWatcher<LibraryRefreshResult>::progressValueChanged, this,
            [this, watcher](int completed) {
                updateLibraryRefreshProgress(completed, watcher->progressMaximum());
            });
    connect(watcher, &QFutureWatcher<LibraryRefreshResult>::finished, this,
            [this, watcher, retryFailedScans] {
        const LibraryRefreshResult result = watcher->result();
        QString error;
        const bool saved = m_repository->applyRefresh(result, error);
        m_libraryRefreshInProgress = false;
        loadSongs();
        scanSongsWithoutArtist(retryFailedScans);
        configureIconButton(m_refreshLibraryButton, QStringLiteral(":/icons/refresh.svg"),
                            QStringLiteral("刷新音乐库"), QSize(16, 16));
        m_refreshLibraryButton->setEnabled(true);
        watcher->deleteLater();
        if (!saved || !result.error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("刷新未完成"), saved ? result.error : error);
        }
    });
    const auto future = QtConcurrent::run(collectLibraryRefresh, songs);
    m_refreshFuture = future;
    watcher->setFuture(future);
}

void MainWindow::updateLibraryRefreshProgress(int completed, int total)
{
    if (m_refreshLibraryButton == nullptr) {
        return;
    }

    const int safeTotal = qMax(1, total);
    const int percent = qBound(0, qRound(completed * 100.0 / safeTotal), 100);
    QPixmap progressPixmap(24, 24);
    progressPixmap.fill(Qt::transparent);
    QPainter painter(&progressPixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF progressRect(2.5, 2.5, 19.0, 19.0);
    painter.setPen(QPen(QColor(174, 179, 199, 80), 2.0));
    painter.drawEllipse(progressRect);
    painter.setPen(QPen(QColor(QStringLiteral("#887aff")), 2.2, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(progressRect, 90 * 16, -qRound(percent * 360.0 / 100.0) * 16);

    QFont progressFont = painter.font();
    progressFont.setPixelSize(percent == 100 ? 7 : 8);
    progressFont.setBold(true);
    painter.setFont(progressFont);
    painter.setPen(QColor(QStringLiteral("#f4f5fb")));
    painter.drawText(progressRect, Qt::AlignCenter, QString::number(percent));
    painter.end();

    m_refreshLibraryButton->setIcon(QIcon(progressPixmap));
    m_refreshLibraryButton->setIconSize(QSize(24, 24));
    m_refreshLibraryButton->setToolTip(
        QStringLiteral("刷新音乐库：%1/%2").arg(qMin(completed, safeTotal)).arg(safeTotal));
    m_refreshLibraryButton->setAccessibleName(m_refreshLibraryButton->toolTip());
}

void MainWindow::addFiles(qint64 groupId)
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择音乐"), {},
        QStringLiteral("音频 (*.mp3 *.wav *.flac *.m4a *.aac *.ogg)"));
    if (!files.isEmpty()) {
        importMusic(files, {}, groupId);
    }
}

void MainWindow::addFolder(qint64 groupId)
{
    const QString folder = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择音乐文件夹"), {},
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!folder.isEmpty()) {
        importMusic({}, folder, groupId);
    }
}

void MainWindow::importMusic(const QStringList &files, const QString &folder, qint64 groupId)
{
    if (m_metadataReadInProgress || m_libraryImportInProgress || m_libraryRefreshInProgress ||
        m_fileEditInProgress) {
        QMessageBox::information(this, QStringLiteral("请稍候"),
                                 QStringLiteral("音乐库正在更新，请完成后再导入。"));
        return;
    }
    m_libraryImportInProgress = true;
    updateTaskbarToolbar();
    m_refreshLibraryButton->setEnabled(false);
    m_metadataScanner->suspend();
    auto *progress = new QProgressDialog(QStringLiteral("正在扫描音频文件…"), {}, 0, 0, this);
    progress->setWindowTitle(QStringLiteral("导入音乐"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setCancelButton(nullptr);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->show();
    auto *watcher = new QFutureWatcher<MusicImportResult>(this);
    connect(watcher, &QFutureWatcher<MusicImportResult>::progressRangeChanged, progress,
            &QProgressDialog::setRange);
    connect(watcher, &QFutureWatcher<MusicImportResult>::progressValueChanged, progress,
            &QProgressDialog::setValue);
    connect(watcher, &QFutureWatcher<MusicImportResult>::progressTextChanged, progress,
            &QProgressDialog::setLabelText);
    connect(watcher, &QFutureWatcher<MusicImportResult>::finished, this, [this, watcher, progress] {
        m_libraryImportInProgress = false;
        m_metadataScanner->resume();
        updateTaskbarToolbar();
        m_refreshLibraryButton->setEnabled(true);
        progress->close();
        progress->deleteLater();
        if (!watcher->isCanceled()) {
            const MusicImportResult result = watcher->result();
            loadSongs();
            if (result.failed > 0 || !result.error.isEmpty()) {
                const QString summary = QStringLiteral("新增 %1 首，重复 %2 首，失败 %3 首。")
                                            .arg(result.added)
                                            .arg(result.duplicates)
                                            .arg(result.failed);
                const QString message = result.error.isEmpty()
                                            ? summary
                                            : result.error + QStringLiteral("\n") + summary;
                QMessageBox::warning(this, QStringLiteral("导入存在失败"), message);
            }
            refreshMusicLibrary();
        }
        watcher->deleteLater();
    });
    const auto future =
        QtConcurrent::run(collectMusicImport, m_database.databaseName(), files, folder, groupId);
    m_importFuture = future;
    watcher->setFuture(future);
}
