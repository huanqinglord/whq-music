#include "main_window.h"
#include "player_widgets.h"
#include "playback_controller.h"

#include <QAbstractItemView>
#include <QAudioOutput>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMenu>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

using namespace PlayerWidgets;

QWidget *MainWindow::createLibraryPage()
{
    auto *content = new QHBoxLayout;
    auto *sidebar = new QVBoxLayout;
    content->setSpacing(0);

    m_groups = new QListWidget(this);
    m_groups->setObjectName(QStringLiteral("groupList"));
    auto *libraryLabel = new QLabel(QStringLiteral("音乐库"), this);
    libraryLabel->setObjectName(QStringLiteral("libraryPageTitle"));
    auto *libraryHeader = new QWidget(this);
    libraryHeader->setObjectName(QStringLiteral("libraryHeader"));
    libraryHeader->setFixedHeight(40);
    auto *libraryTitleLayout = new QHBoxLayout(libraryHeader);
    libraryTitleLayout->setContentsMargins(0, 0, 0, 0);
    libraryTitleLayout->addWidget(libraryLabel);
    libraryTitleLayout->addStretch();
    sidebar->addWidget(libraryHeader);
    sidebar->addWidget(m_groups);

    auto *libraryPanel = new QWidget(this);
    libraryPanel->setObjectName(QStringLiteral("panel"));
    libraryPanel->setLayout(sidebar);

    auto *songTable = new SongTableView(this);
    songTable->setItemDelegate(new SongTableItemDelegate(songTable));
    m_songsTable = songTable;
    m_songsTable->setObjectName(QStringLiteral("songTable"));
    QPalette songTablePalette = m_songsTable->palette();
    songTablePalette.setColor(QPalette::Highlight, QColor(0, 0, 0, 0));
    songTablePalette.setColor(QPalette::Inactive, QPalette::Highlight, QColor(0, 0, 0, 0));
    songTablePalette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#dfe2ee")));
    songTablePalette.setColor(QPalette::Inactive, QPalette::HighlightedText,
                              QColor(QStringLiteral("#dfe2ee")));
    m_songsTable->setPalette(songTablePalette);
    m_songModel = new SongTableModel(m_songs, m_fileSizeCache, m_songsTable);
    m_songsTable->setModel(m_songModel);
    m_songsTable->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_songsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_songsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_songsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_songsTable->horizontalHeader()->setResizeContentsPrecision(0);
    m_songsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_songsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_songsTable->setFocusPolicy(Qt::NoFocus);
    m_songsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_songsTable->setShowGrid(false);
    m_songsTable->setAlternatingRowColors(false);
    m_songsTable->verticalHeader()->setVisible(false);
    m_songsTable->verticalHeader()->setDefaultSectionSize(48);
    m_songsTable->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *songsLayout = new QVBoxLayout;
    auto *songsTitle = new QLabel(QStringLiteral("歌曲"), this);
    songsTitle->setObjectName(QStringLiteral("libraryPageTitle"));
    auto *songsHintButton = new QPushButton(this);
    songsHintButton->setObjectName(QStringLiteral("songsHintButton"));
    configureIconButton(songsHintButton, QStringLiteral(":/icons/info.svg"),
                        QStringLiteral("双击歌曲开始播放，右键可管理分组"), QSize(16, 16));
    m_refreshLibraryButton = new QPushButton(this);
    m_refreshLibraryButton->setObjectName(QStringLiteral("refreshLibraryButton"));
    configureIconButton(m_refreshLibraryButton, QStringLiteral(":/icons/refresh.svg"),
                        QStringLiteral("刷新音乐库"), QSize(16, 16));
    m_searchEditor = new QLineEdit(this);
    m_searchEditor->setObjectName(QStringLiteral("songSearchEditor"));
    m_searchEditor->setPlaceholderText(QStringLiteral("搜索歌曲或歌手"));
    m_searchEditor->setClearButtonEnabled(true);
    auto *songsHeader = new QWidget(this);
    songsHeader->setObjectName(QStringLiteral("songsHeader"));
    songsHeader->setFixedHeight(40);
    auto *songsHeaderLayout = new QHBoxLayout(songsHeader);
    songsHeaderLayout->setContentsMargins(0, 0, 0, 0);
    songsHeaderLayout->setSpacing(6);
    songsHeaderLayout->addWidget(songsTitle);
    songsHeaderLayout->addWidget(songsHintButton);
    songsHeaderLayout->addWidget(m_refreshLibraryButton);
    songsHeaderLayout->addStretch();
    songsHeaderLayout->addWidget(m_searchEditor);
    songsLayout->addWidget(songsHeader);
    songsLayout->addWidget(m_songsTable);
    auto *songsPanel = new QWidget(this);
    songsPanel->setObjectName(QStringLiteral("panel"));
    songsPanel->setLayout(songsLayout);

    content->addWidget(libraryPanel, 2);
    content->addWidget(songsPanel, 6);

    auto *libraryPage = new QWidget(this);
    libraryPage->setObjectName(QStringLiteral("libraryPage"));
    auto *libraryPageLayout = new QVBoxLayout(libraryPage);
    libraryPageLayout->setContentsMargins(0, 0, 0, 0);
    libraryPageLayout->setSpacing(0);
    libraryPageLayout->addLayout(content, 1);

    return libraryPage;
}

QWidget *MainWindow::createNowPlayingPage()
{
    auto *nowPlayingPage = new QWidget(this);
    nowPlayingPage->setObjectName(QStringLiteral("nowPlayingPage"));
    auto *nowPlayingLayout = new QHBoxLayout(nowPlayingPage);
    nowPlayingLayout->setContentsMargins(0, 0, 0, 0);
    nowPlayingLayout->setSpacing(0);
    auto *songInfoLayout = new QVBoxLayout;
    songInfoLayout->setAlignment(Qt::AlignCenter);
    m_cover = new QLabel(this);
    m_cover->setObjectName(QStringLiteral("coverArt"));
    m_cover->setFixedSize(260, 260);
    m_cover->setAlignment(Qt::AlignCenter);
    m_cover->setText(QStringLiteral("♫"));
    m_detailTitle = new QLabel(QStringLiteral("选择一首歌曲开始播放"), this);
    m_detailTitle->setObjectName(QStringLiteral("detailTitle"));
    m_detailTitle->setAlignment(Qt::AlignCenter);
    m_detailTitle->setWordWrap(true);
    m_artist = new QLabel(QStringLiteral("未知歌手"), this);
    m_artist->setObjectName(QStringLiteral("detailArtist"));
    m_artist->setAlignment(Qt::AlignCenter);
    m_album = new QLabel(QStringLiteral("未知专辑"), this);
    m_album->setObjectName(QStringLiteral("mutedLabel"));
    m_album->setAlignment(Qt::AlignCenter);
    songInfoLayout->addStretch();
    songInfoLayout->addWidget(m_cover, 0, Qt::AlignCenter);
    songInfoLayout->addSpacing(20);
    songInfoLayout->addWidget(m_detailTitle);
    songInfoLayout->addWidget(m_artist);
    songInfoLayout->addWidget(m_album);
    songInfoLayout->addStretch();
    auto *songInfoPanel = new QWidget(this);
    songInfoPanel->setObjectName(QStringLiteral("songInfoPanel"));
    songInfoPanel->setLayout(songInfoLayout);

    auto *lyricsLayout = new QVBoxLayout;
    m_lyrics = new LyricsListWidget(this);
    m_lyrics->setObjectName(QStringLiteral("lyricsList"));
    m_lyrics->setFocusPolicy(Qt::NoFocus);
    m_lyrics->setSelectionMode(QAbstractItemView::NoSelection);
    m_lyrics->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lyrics->setResizeMode(QListView::Adjust);
    m_lyrics->setMouseTracking(true);
    m_lyrics->setItemDelegate(new LyricsItemDelegate(m_lyrics));
    m_lyrics->addItem(QStringLiteral("暂无歌词"));
    auto *lyricsHeaderLayout = new QHBoxLayout;
    lyricsHeaderLayout->setContentsMargins(0, 0, 0, 0);
    lyricsHeaderLayout->addStretch();
    m_lyricsScriptButton = new LyricsScriptButton(this);
    m_lyricsScriptButton->setObjectName(QStringLiteral("lyricsScriptButton"));
    m_lyricsButtonPaletteTimer = new QTimer(this);
    m_lyricsButtonPaletteTimer->setSingleShot(true);
    m_lyricsButtonPaletteTimer->setInterval(180);
    connect(m_lyricsButtonPaletteTimer, &QTimer::timeout, this, [this] {
        updateLyricsScriptButtonPalette(m_backgroundPath);
        updateCaptionButtonPalette(m_backgroundPath);
    });
    lyricsHeaderLayout->addWidget(m_lyricsScriptButton);
    m_desktopLyricsButton = new LyricsScriptButton(this);
    m_desktopLyricsButton->setText(QStringLiteral("桌"));
    m_desktopLyricsButton->setObjectName(QStringLiteral("desktopLyricsButton"));
    m_desktopLyricsButton->setToolTip(QStringLiteral("桌面歌词"));
    m_desktopLyricsButton->setAccessibleName(QStringLiteral("桌面歌词"));
    m_desktopLyricsButton->setCheckable(true);
    m_desktopLyricsButton->setContextMenuPolicy(Qt::CustomContextMenu);
    lyricsHeaderLayout->addWidget(m_desktopLyricsButton);
    connect(m_desktopLyricsButton, &QPushButton::clicked, this, &MainWindow::toggleDesktopLyrics);
    connect(m_desktopLyricsButton, &QPushButton::customContextMenuRequested, this,
            [this](const QPoint &position) {
                QMenu menu(this);
                menu.addAction(QStringLiteral("显示 / 隐藏桌面歌词"), this,
                               &MainWindow::toggleDesktopLyrics);
                menu.exec(m_desktopLyricsButton->mapToGlobal(position));
            });
    lyricsLayout->addLayout(lyricsHeaderLayout);
    lyricsLayout->addWidget(m_lyrics, 1);
    auto *lyricsPanel = new QWidget(this);
    lyricsPanel->setObjectName(QStringLiteral("lyricsPanel"));
    lyricsPanel->setLayout(lyricsLayout);

    nowPlayingLayout->addWidget(songInfoPanel, 3);
    nowPlayingLayout->addWidget(lyricsPanel, 7);

    return nowPlayingPage;
}

void MainWindow::connectLibraryControls()
{
    connect(m_libraryButton, &QPushButton::clicked, this, &MainWindow::toggleLibrary);
    connect(m_refreshLibraryButton, &QPushButton::clicked, this, [this] {
        refreshMusicLibrary(true);
    });
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(200);
    connect(m_searchEditor, &QLineEdit::textChanged, this, [this] {
        m_searchTimer->start();
    });
    connect(m_searchTimer, &QTimer::timeout, this, [this] {
        loadSongs(false);
    });
    connect(m_groups, &QListWidget::currentRowChanged, this, [this](int row) {
        QListWidgetItem *item = row >= 0 ? m_groups->item(row) : nullptr;
        if (item == nullptr || !item->flags().testFlag(Qt::ItemIsSelectable)) {
            QSignalBlocker selectionBlocker(m_groups);
            for (int itemRow = 0; itemRow < m_groups->count(); ++itemRow) {
                QListWidgetItem *candidate = m_groups->item(itemRow);
                if (candidate->data(Qt::UserRole).toLongLong() == m_activeGroupId) {
                    m_groups->setCurrentItem(candidate);
                    break;
                }
            }
            return;
        }
        m_activeGroupId = item->data(Qt::UserRole).toLongLong();
        loadSongs(false);
    });
    connect(m_songsTable, &QTableView::customContextMenuRequested, this,
            &MainWindow::showSongMenu);
    connect(m_queue, &QListWidget::customContextMenuRequested, this, &MainWindow::showQueueMenu);
    connect(m_songsTable, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        playRow(index.row());
    });
    connect(m_queue, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        playQueueRow(m_queue->row(item));
    });
    connect(m_lyrics, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (item->data(Qt::UserRole + 2).toBool()) {
            requestLyricsDownload(m_missingLyricsAudioPath);
        }
    });
    connect(m_lyrics, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item->data(Qt::UserRole + 2).toBool()) {
            return;
        }
        const QVariant timestamp = item->data(Qt::UserRole);
        if (timestamp.isValid() && timestamp.toLongLong() >= 0) {
            m_playbackController->seekTo(timestamp.toLongLong());
            m_progress->setValue(static_cast<int>(timestamp.toLongLong()));
        }
    });
    connect(m_lyricsScriptButton, &QPushButton::clicked, this, [this] {
        const bool simplifiedLyrics = !m_showSimplifiedLyrics;
        if (!saveSetting(QStringLiteral("simplifiedLyrics"),
                         simplifiedLyrics ? QStringLiteral("1") : QStringLiteral("0"))) {
            return;
        }
        m_showSimplifiedLyrics = simplifiedLyrics;
        updateLyricsScriptButton();
        if (m_player->source().isLocalFile()) {
            loadLyrics(m_player->source().toLocalFile());
            updateLyrics(m_player->position());
        }
    });
}
