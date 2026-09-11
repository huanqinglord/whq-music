#include "main_window.h"
#include "player_widgets.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QAudioOutput>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMenu>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidgetAction>

using namespace PlayerWidgets;

void MainWindow::buildUi()
{
    auto *central = new AppSurface(this);
    central->setObjectName(QStringLiteral("appSurface"));
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    setCentralWidget(central);
    createTitleBar();
    root->addWidget(m_titleBar);
    QWidget *libraryPage = createLibraryPage();
    QWidget *nowPlayingPage = createNowPlayingPage();
    QWidget *settingsPage = createSettingsPage();
    m_contentStack = new QStackedWidget(this);
    m_contentStack->setObjectName(QStringLiteral("contentStack"));
    m_contentStack->addWidget(nowPlayingPage);
    m_contentStack->addWidget(libraryPage);
    m_contentStack->addWidget(settingsPage);
    m_contentStack->setCurrentWidget(nowPlayingPage);

    createPlayerBar();
    root->addWidget(m_contentStack, 1);
    root->addWidget(m_playerBar);
    central->setBlurTargets(m_contentStack, m_playerBar);
    connect(m_contentStack, &QStackedWidget::currentChanged, central, [central](int index) {
        central->setContentBlurred(index != 0);
    });
    QApplication::instance()->installEventFilter(new FramelessWindowEventFilter(this, m_titleBar));
    QApplication::instance()->installEventFilter(this);
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state != Qt::ApplicationActive && m_queueDrawerOpen) {
                    toggleQueueDrawer();
                }
            });

    initializePlayback();
    connectLibraryControls();
}

void MainWindow::createTitleBar()
{
    m_titleBar = new QWidget(this);
    m_titleBar->setObjectName(QStringLiteral("titleBar"));
    auto *titleBarLayout = new QHBoxLayout(m_titleBar);
    titleBarLayout->setContentsMargins(12, 0, 14, 0);
    titleBarLayout->setSpacing(10);
    auto *settingsButton = new QPushButton(m_titleBar);
    settingsButton->setObjectName(QStringLiteral("captionButton"));
    auto *minimizeButton = new QPushButton(m_titleBar);
    minimizeButton->setObjectName(QStringLiteral("captionButton"));
    m_maximizeButton = new QPushButton(m_titleBar);
    m_maximizeButton->setObjectName(QStringLiteral("captionButton"));
    auto *closeButton = new QPushButton(m_titleBar);
    closeButton->setObjectName(QStringLiteral("closeCaptionButton"));
    configureIconButton(settingsButton, QStringLiteral(":/icons/settings.svg"),
                        QStringLiteral("设置"), QSize(18, 18));
    configureIconButton(minimizeButton, QStringLiteral(":/icons/minimize.svg"),
                        QStringLiteral("最小化"), QSize(18, 18));
    configureIconButton(m_maximizeButton, QStringLiteral(":/icons/maximize.svg"),
                        QStringLiteral("最大化"), QSize(18, 18));
    configureIconButton(closeButton, QStringLiteral(":/icons/close.svg"),
                        QStringLiteral("最小化到托盘"), QSize(18, 18));
    m_captionButtons = {settingsButton, minimizeButton, m_maximizeButton, closeButton};
    closeButton->installEventFilter(new CaptionIconHoverFilter(closeButton));
    titleBarLayout->addStretch();
    titleBarLayout->addWidget(settingsButton);
    titleBarLayout->addWidget(minimizeButton);
    titleBarLayout->addWidget(m_maximizeButton);
    titleBarLayout->addWidget(closeButton);

    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::showSettings);
    connect(minimizeButton, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(m_maximizeButton, &QPushButton::clicked, this, &MainWindow::toggleMaximized);
    connect(closeButton, &QPushButton::clicked, this, &QWidget::close);
}

void MainWindow::createPlayerBar()
{
    auto *central = static_cast<AppSurface *>(centralWidget());
    auto *previousButton = new QPushButton(QStringLiteral("上一曲"), this);
    m_libraryButton = new QPushButton(QStringLiteral("音乐库与分组"), this);
    m_libraryButton->setObjectName(QStringLiteral("libraryButton"));
    m_playButton = new QPushButton(QStringLiteral("播放"), this);
    m_playButton->setObjectName(QStringLiteral("playButton"));
    auto *nextButton = new QPushButton(QStringLiteral("下一曲"), this);
    m_modeButton = new QPushButton(QStringLiteral("列表循环"), this);
    m_modeButton->setObjectName(QStringLiteral("modeButton"));
    m_progress = new PlaybackProgressSlider(this);
    m_time = new QLabel(QStringLiteral("00:00 / 00:00"), this);
    auto *volumeButton = new QPushButton(QStringLiteral("音量"), this);
    volumeButton->setObjectName(QStringLiteral("volumeButton"));
    m_queueButton = new QPushButton(QStringLiteral("播放列表"), this);
    m_queueButton->setObjectName(QStringLiteral("queueButton"));
    m_audioEffectButton = new QPushButton(this);
    m_audioEffectButton->setObjectName(QStringLiteral("audioEffectButton"));
    configureIconButton(previousButton, QStringLiteral(":/icons/previous.svg"),
                        QStringLiteral("上一曲"));
    configureIconButton(m_libraryButton, QStringLiteral(":/icons/music_library.svg"),
                        QStringLiteral("音乐库与分组"));
    configureIconButton(m_playButton, QStringLiteral(":/icons/play.svg"), QStringLiteral("播放"),
                        QSize(24, 24));
    configureIconButton(nextButton, QStringLiteral(":/icons/next.svg"), QStringLiteral("下一曲"));
    configureIconButton(volumeButton, QStringLiteral(":/icons/speaker.svg"),
                        QStringLiteral("音量"));
    configureIconButton(m_queueButton, QStringLiteral(":/icons/queue.svg"),
                        QStringLiteral("播放列表"));
    configureIconButton(m_audioEffectButton, QStringLiteral(":/icons/equalizer.svg"),
                        QStringLiteral("原声"));

    m_audioEffectMenu = new QMenu(this);
    m_audioEffectMenu->setObjectName(QStringLiteral("playerPopup"));
    auto *effectGroup = new QActionGroup(m_audioEffectMenu);
    effectGroup->setExclusive(true);
    const QList<QPair<QString, AudioEffectProcessor::Preset>> effects = {
        {QStringLiteral("原声"), AudioEffectProcessor::Preset::Original},
        {QStringLiteral("清澈人声"), AudioEffectProcessor::Preset::ClearVocal},
        {QStringLiteral("低音增强"), AudioEffectProcessor::Preset::BassBoost},
        {QStringLiteral("现场"), AudioEffectProcessor::Preset::Live}};
    for (const auto &[name, preset] : effects) {
        QAction *action = m_audioEffectMenu->addAction(name);
        action->setCheckable(true);
        action->setData(static_cast<int>(preset));
        effectGroup->addAction(action);
    }

    auto *volumeMenu = new QMenu(this);
    volumeMenu->setObjectName(QStringLiteral("playerPopup"));
    auto *volumePanel = new QWidget(volumeMenu);
    auto *volumeLayout = new QVBoxLayout(volumePanel);
    volumeLayout->setContentsMargins(14, 14, 14, 14);
    m_volume = new QSlider(Qt::Vertical, volumePanel);
    m_volume->setRange(0, 100);
    m_volume->setFixedHeight(150);
    volumeLayout->addWidget(m_volume, 0, Qt::AlignHCenter);
    auto *volumeAction = new QWidgetAction(volumeMenu);
    volumeAction->setDefaultWidget(volumePanel);
    volumeMenu->addAction(volumeAction);

    m_queueDrawer = new QueueDrawer(central);
    static_cast<QueueDrawer *>(m_queueDrawer)
        ->setBackdropProvider([central](const QWidget *target) {
            return central->blurredRegion(target);
        });
    m_queueDrawer->setObjectName(QStringLiteral("queueDrawer"));
    auto *queueLayout = new QVBoxLayout(m_queueDrawer);
    queueLayout->setContentsMargins(22, 22, 0, 20);
    queueLayout->setSpacing(8);
    auto *queueTitle = new QLabel(QStringLiteral("播放列表"), m_queueDrawer);
    queueTitle->setObjectName(QStringLiteral("sectionLabel"));
    m_queueSearchEditor = new QLineEdit(m_queueDrawer);
    m_queueSearchEditor->setObjectName(QStringLiteral("queueSearchEditor"));
    m_queueSearchEditor->setPlaceholderText(QStringLiteral("搜索播放列表"));
    m_queueSearchEditor->setAccessibleName(QStringLiteral("搜索播放列表"));
    m_queueSearchEditor->setClearButtonEnabled(true);
    auto *queueHeaderLayout = new QHBoxLayout;
    queueHeaderLayout->setContentsMargins(0, 0, 22, 0);
    queueHeaderLayout->setSpacing(12);
    queueHeaderLayout->addWidget(queueTitle);
    queueHeaderLayout->addWidget(m_queueSearchEditor, 1);
    m_queue = new QListWidget(m_queueDrawer);
    m_queue->setObjectName(QStringLiteral("queueList"));
    m_queue->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_queue->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_queue->setTextElideMode(Qt::ElideRight);
    m_queue->setWordWrap(false);
    m_queue->setResizeMode(QListView::Adjust);
    m_queue->setContextMenuPolicy(Qt::CustomContextMenu);
    queueLayout->addLayout(queueHeaderLayout);
    queueLayout->addWidget(m_queue);
    connect(m_queueSearchEditor, &QLineEdit::textChanged, this,
            &MainWindow::filterPlaybackQueue);
    m_queueDrawer->hide();
    m_queueDrawerAnimation = new QPropertyAnimation(m_queueDrawer, "geometry", this);
    m_queueDrawerAnimation->setDuration(220);
    connect(m_queueDrawerAnimation, &QPropertyAnimation::valueChanged, m_queueDrawer, [this] {
        m_queueDrawer->update();
    });
    connect(m_queueDrawerAnimation, &QPropertyAnimation::finished, this, [this] {
        if (!m_queueDrawerOpen) {
            m_queueDrawer->hide();
        }
    });

    m_playerBar = new QWidget(this);
    m_playerBar->setObjectName(QStringLiteral("playerBar"));
    auto *playerBarLayout = new QVBoxLayout(m_playerBar);
    auto *progressLayout = new QHBoxLayout;
    progressLayout->setSpacing(12);
    progressLayout->addWidget(m_progress, 1);
    progressLayout->addWidget(m_time);
    auto *leftControls = new QHBoxLayout;
    auto *centerControls = new QHBoxLayout;
    auto *rightControls = new QHBoxLayout;
    leftControls->addWidget(m_libraryButton);
    leftControls->addWidget(m_audioEffectButton);
    leftControls->addStretch();
    centerControls->addStretch();
    centerControls->addWidget(previousButton);
    centerControls->addWidget(m_playButton);
    centerControls->addWidget(nextButton);
    centerControls->addStretch();
    rightControls->addStretch();
    rightControls->addWidget(m_modeButton);
    rightControls->addWidget(volumeButton);
    rightControls->addWidget(m_queueButton);
    auto *controls = new QHBoxLayout;
    controls->setSpacing(12);
    controls->addLayout(leftControls, 1);
    controls->addLayout(centerControls, 1);
    controls->addLayout(rightControls, 1);
    playerBarLayout->addLayout(progressLayout);
    playerBarLayout->addLayout(controls);
    connect(m_playButton, &QPushButton::clicked, this, &MainWindow::togglePlayback);
    connect(previousButton, &QPushButton::clicked, this, &MainWindow::playPrevious);
    connect(nextButton, &QPushButton::clicked, this, [this] {
        playNext();
    });
    connect(volumeButton, &QPushButton::clicked, this, [volumeButton, volumeMenu] {
        volumeMenu->adjustSize();
        const QPoint popupPosition = volumeButton->mapToGlobal(
            QPoint((volumeButton->width() - volumeMenu->width()) / 2, -volumeMenu->height()));
        volumeMenu->popup(popupPosition);
    });
    connect(m_queueButton, &QPushButton::clicked, this, &MainWindow::toggleQueueDrawer);
    qApp->installEventFilter(new PopupToggleEventFilter(m_audioEffectButton, m_audioEffectMenu));
    connect(m_audioEffectButton, &QPushButton::clicked, this, [this] {
        if (m_audioEffectMenu->isVisible()) {
            m_audioEffectMenu->hide();
            return;
        }
        const QPoint popupPosition =
            m_audioEffectButton->mapToGlobal(QPoint(0, -m_audioEffectMenu->sizeHint().height()));
        m_audioEffectMenu->popup(popupPosition);
    });
    connect(m_audioEffectMenu, &QMenu::triggered, this, [this](QAction *action) {
        applyAudioEffect(static_cast<AudioEffectProcessor::Preset>(action->data().toInt()));
    });
    connect(m_modeButton, &QPushButton::clicked, this, &MainWindow::cyclePlayMode);
}
