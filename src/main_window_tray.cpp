#include "main_window.h"
#include "desktop_lyrics_window.h"
#include "player_widgets.h"
#include "playback_controller.h"

#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMediaPlayer>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScopeGuard>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidgetAction>

#include <array>

#ifdef Q_OS_WIN
#include <shobjidl.h>
#include <windows.h>
#include <dwmapi.h>

#endif

using namespace PlayerWidgets;

#ifdef Q_OS_WIN
namespace {
enum class TaskbarCommand : unsigned int
{
    Previous = 0x4001,
    PlayPause,
    Next,
    PlayMode,
    DesktopLyrics
};
}
#endif

void MainWindow::initializeTaskbarToolbar()
{
#ifdef Q_OS_WIN
    updateTaskbarBackground();
    // Explorer 重启或窗口重新进入任务栏后，旧代理和按钮注册都可能失效。
    m_taskbarButtonsAdded = false;
    if (m_taskbar != nullptr) {
        m_taskbar->Release();
        m_taskbar = nullptr;
    }
    if (!m_taskbarComInitialized) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(initialized)) {
            m_taskbarComInitialized = true;
        } else if (initialized != RPC_E_CHANGED_MODE) {
            qWarning() << "Failed to initialize taskbar COM" << initialized;
            return;
        }
    }
    const HRESULT created = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&m_taskbar));
    if (FAILED(created)) {
        qWarning() << "Failed to create taskbar toolbar" << created;
        return;
    }
    const HRESULT initialized = m_taskbar->HrInit();
    if (FAILED(initialized)) {
        qWarning() << "Failed to initialize taskbar toolbar" << initialized;
        m_taskbar->Release();
        m_taskbar = nullptr;
        return;
    }
    updateTaskbarToolbar();
#endif
}

void MainWindow::updateTaskbarToolbar()
{
    updateTrayPlayback();
#ifdef Q_OS_WIN
    if (m_taskbar == nullptr || m_player == nullptr || m_modeButton == nullptr || !isVisible()) {
        return;
    }
    const bool playing = m_playbackController != nullptr && m_playbackController->isPlaying();
    const bool hasSource = !m_player->source().isEmpty();
    const bool hasSongs = !m_playbackController->queue().isEmpty() || !m_songs.isEmpty();
    const bool controlsEnabled =
        isEnabled() && !m_exitRequested && !m_libraryImportInProgress && !m_fileEditInProgress;
    const bool lyricsVisible = m_desktopLyrics != nullptr && m_desktopLyrics->isVisible();
    const int iconSize = qMax(16, GetSystemMetrics(SM_CXSMICON));
    QPixmap lyricsPixmap(iconSize, iconSize);
    lyricsPixmap.fill(Qt::transparent);
    {
        QPainter painter(&lyricsPixmap);
        painter.setRenderHint(QPainter::TextAntialiasing);
        QFont font = QApplication::font();
        font.setPixelSize(iconSize - 2);
        painter.setFont(font);
        painter.setPen(QColor(QStringLiteral("#d9dbea")));
        painter.drawText(lyricsPixmap.rect(), Qt::AlignCenter, QStringLiteral("桌"));
    }
    const std::array<TaskbarCommand, 5> commands = {
        TaskbarCommand::Previous, TaskbarCommand::PlayPause, TaskbarCommand::Next,
        TaskbarCommand::PlayMode, TaskbarCommand::DesktopLyrics};
    const std::array<QIcon, 5> icons = {
        QIcon(QStringLiteral(":/icons/previous.svg")),
        QIcon(playing ? QStringLiteral(":/icons/pause.svg") : QStringLiteral(":/icons/play.svg")),
        QIcon(QStringLiteral(":/icons/next.svg")), m_modeButton->icon(), QIcon(lyricsPixmap)};
    const std::array<QString, 5> tooltips = {
        QStringLiteral("上一曲"), playing ? QStringLiteral("暂停") : QStringLiteral("播放"),
        QStringLiteral("下一曲"),
        QStringLiteral("播放模式：%1（点击切换）").arg(m_modeButton->toolTip()),
        lyricsVisible ? QStringLiteral("隐藏桌面歌词") : QStringLiteral("显示桌面歌词")};
    const std::array<bool, 5> enabled = {
        hasSongs || hasSource, hasSongs || hasSource, hasSongs, true, m_desktopLyrics != nullptr};
    std::array<THUMBBUTTON, 5> buttons{};
    const auto releaseIcons = qScopeGuard([&buttons] {
        for (const THUMBBUTTON &button : buttons) {
            if (button.hIcon != nullptr) {
                DestroyIcon(button.hIcon);
            }
        }
    });
    for (size_t index = 0; index < buttons.size(); ++index) {
        THUMBBUTTON &button = buttons[index];
        button.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        button.iId = static_cast<UINT>(commands[index]);
        button.hIcon = icons[index].pixmap(iconSize, iconSize).toImage().toHICON();
        if (button.hIcon == nullptr) {
            qWarning() << "Failed to create taskbar button icon" << button.iId;
            return;
        }
        tooltips[index].left(259).toWCharArray(button.szTip);
        // 不使用 DISMISSONCLICK，方便用户在预览窗口中连续切歌或切换模式。
        button.dwFlags = controlsEnabled && enabled[index] ? THBF_ENABLED : THBF_DISABLED;
    }
    const HWND window = reinterpret_cast<HWND>(winId());
    const UINT count = static_cast<UINT>(buttons.size());
    const HRESULT updated = m_taskbarButtonsAdded
                                ? m_taskbar->ThumbBarUpdateButtons(window, count, buttons.data())
                                : m_taskbar->ThumbBarAddButtons(window, count, buttons.data());
    if (FAILED(updated)) {
        qWarning() << "Failed to update taskbar playback buttons" << updated;
        return;
    }
    m_taskbarButtonsAdded = true;
#endif
}

void MainWindow::updateTaskbarBackground()
{
#ifdef Q_OS_WIN
    // 缓存图片，系统请求预览时不抓取屏幕，也不重复读取背景文件。
    if (m_taskbarBackgroundPath != m_backgroundPath || m_taskbarBackground.isNull()) {
        m_taskbarBackground = loadScaledImage(m_backgroundPath, QSize(2048, 2048));
        if (m_taskbarBackground.isNull()) {
            qWarning() << "Failed to read taskbar background";
            m_taskbarBackground = loadScaledImage(kDefaultBackgroundPath, QSize(2048, 2048));
        }
        m_taskbarBackgroundPath = m_backgroundPath;
    }

    const HWND window = reinterpret_cast<HWND>(winId());
    const BOOL enabled = TRUE;
    for (const auto attribute : {DWMWA_FORCE_ICONIC_REPRESENTATION, DWMWA_HAS_ICONIC_BITMAP}) {
        const HRESULT status = DwmSetWindowAttribute(window, attribute, &enabled, sizeof(enabled));
        if (FAILED(status)) {
            qWarning() << "Failed to configure taskbar background" << attribute << status;
            return;
        }
    }
    const HRESULT status = DwmInvalidateIconicBitmaps(window);
    if (FAILED(status)) {
        qWarning() << "Failed to refresh taskbar background" << status;
    }
#endif
}

#ifdef Q_OS_WIN
bool MainWindow::publishTaskbarBackground(const QSize &maximumSize, bool livePreview)
{
    if (maximumSize.isEmpty()) {
        return false;
    }
    const QSize imageSize = m_taskbarBackground.isNull()
                                ? maximumSize
                                : m_taskbarBackground.size().scaled(maximumSize, Qt::KeepAspectRatio);
    QImage preview(livePreview ? maximumSize : imageSize, QImage::Format_ARGB32_Premultiplied);
    if (preview.isNull()) {
        qWarning() << "Failed to allocate taskbar background bitmap";
        return false;
    }
    preview.fill(QColor(QStringLiteral("#0b0d14")));
    if (!m_taskbarBackground.isNull()) {
        QPainter painter(&preview);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QPoint origin((preview.width() - imageSize.width()) / 2,
                            (preview.height() - imageSize.height()) / 2);
        painter.drawImage(QRect(origin, imageSize), m_taskbarBackground);
    }
    const HBITMAP bitmap = preview.toHBITMAP();
    if (bitmap == nullptr) {
        qWarning() << "Failed to create taskbar background bitmap" << GetLastError();
        return false;
    }
    const auto releaseBitmap = qScopeGuard([bitmap] {
        DeleteObject(bitmap);
    });
    const HWND window = reinterpret_cast<HWND>(winId());
    const HRESULT status = livePreview
                               ? DwmSetIconicLivePreviewBitmap(window, bitmap, nullptr, 0)
                               : DwmSetIconicThumbnail(window, bitmap, 0);
    if (FAILED(status)) {
        qWarning() << "Failed to publish taskbar background" << livePreview << status;
        return false;
    }
    return true;
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    const auto *nativeMessage = static_cast<MSG *>(message);
    if (nativeMessage != nullptr) {
        if (nativeMessage->message == WM_DWMSENDICONICTHUMBNAIL) {
            const QSize maximumSize(HIWORD(nativeMessage->lParam), LOWORD(nativeMessage->lParam));
            if (publishTaskbarBackground(maximumSize, false)) {
                *result = 0;
                return true;
            }
        } else if (nativeMessage->message == WM_DWMSENDICONICLIVEPREVIEWBITMAP) {
            RECT clientRect{};
            if (GetClientRect(nativeMessage->hwnd, &clientRect) &&
                publishTaskbarBackground(QSize(clientRect.right - clientRect.left,
                                              clientRect.bottom - clientRect.top), true)) {
                *result = 0;
                return true;
            }
        } else if (nativeMessage->message == WM_DWMCOMPOSITIONCHANGED) {
            updateTaskbarBackground();
        }
    }
    if (nativeMessage != nullptr && m_taskbarCreatedMessage != 0 &&
        nativeMessage->message == m_taskbarCreatedMessage) {
        initializeTaskbarToolbar();
    } else if (nativeMessage != nullptr && nativeMessage->message == WM_COMMAND &&
               HIWORD(nativeMessage->wParam) == THBN_CLICKED) {
        const auto command = static_cast<TaskbarCommand>(LOWORD(nativeMessage->wParam));
        switch (command) {
        case TaskbarCommand::Previous:
        case TaskbarCommand::PlayPause:
        case TaskbarCommand::Next:
        case TaskbarCommand::PlayMode:
        case TaskbarCommand::DesktopLyrics:
            // 退出原生消息处理后再执行按钮命令，避免 COM 回调中发生嵌套操作。
            QTimer::singleShot(0, this, [this, command] {
                if (!isEnabled() || m_exitRequested) {
                    return;
                }
                switch (command) {
                case TaskbarCommand::Previous:
                    playPrevious();
                    break;
                case TaskbarCommand::PlayPause:
                    togglePlayback();
                    break;
                case TaskbarCommand::Next:
                    playNext();
                    break;
                case TaskbarCommand::PlayMode:
                    cyclePlayMode();
                    break;
                case TaskbarCommand::DesktopLyrics:
                    toggleDesktopLyrics();
                    break;
                }
            });
            *result = 0;
            return true;
        default:
            break;
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void MainWindow::initializeSystemTray()
{
    m_trayIcon = new QSystemTrayIcon(QIcon(QStringLiteral(":/icons/app_icon.png")), this);
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("trayPlaybackMenu"));
    menu->setStyleSheet(QStringLiteral(
        "QMenu#trayPlaybackMenu { background: #fff7ea; border: 1px solid #d8cfe0; padding: 0; }"
        "QWidget#trayPlaybackPanel, QWidget#trayPlaybackPanel * { font-family: 'KaiTi';"
        " font-size: 15px; color: #000000; }"
        "QWidget#trayPlaybackPanel QLabel { background: transparent; }"
        "QWidget#trayPlaybackPanel QPushButton { background: rgba(255, 250, 245, 190);"
        " border: none; border-radius: 16px; padding: 0; min-width: 0; min-height: 0; }"
        "QWidget#trayPlaybackPanel QPushButton:hover { background: #fffdf9; }"
        "QWidget#trayPlaybackPanel QPushButton:pressed { background: #eee5f5; }"
        "QWidget#trayPlaybackPanel QPushButton:disabled { color: #777777; }"
        "QWidget#trayPlaybackPanel QPushButton#trayDesktopLyrics:checked { background: #e6dafa; }"
        "QWidget#trayPlaybackPanel QPushButton#trayPrimaryPlay { background: #e6dafa;"
        " border-radius: 22px; }"
        "QWidget#trayPlaybackPanel QPushButton#trayPrimaryPlay:hover { background: #f1e9ff; }"
        "QWidget#trayPlaybackPanel QPushButton#trayPrimaryPlay:pressed { background: #d9caef; }"
        "QWidget#trayPlaybackPanel QPushButton#trayPrimaryPlay:disabled { background: #eee8e2; }"
        "QWidget#trayPlaybackPanel QPushButton#trayMode,"
        "QWidget#trayPlaybackPanel QPushButton#trayDesktopLyrics { background: transparent; border-radius: "
        "8px; }"
        "QWidget#trayPlaybackPanel QPushButton#trayMode:hover,"
        "QWidget#trayPlaybackPanel QPushButton#trayDesktopLyrics:hover { background: rgba(90, 65, 110, 18); "
        "}"
        "QWidget#trayPlaybackPanel QPushButton#trayMode:pressed,"
        "QWidget#trayPlaybackPanel QPushButton#trayDesktopLyrics:pressed { background: rgba(90, 65, 110, "
        "28); }"
        "QWidget#trayPlaybackPanel QPushButton#trayExit { background: transparent; border-radius: "
        "6px;"
        " text-align: left; padding-left: 9px; }"
        "QWidget#trayPlaybackPanel QPushButton#trayExit:hover { background: rgba(90, 65, 110, 18); "
        "}"
        "QWidget#trayPlaybackPanel QPushButton#trayExit:pressed { background: rgba(90, 65, 110, "
        "28); }"));

    auto *panel = new TrayPlaybackPanel(menu);
    panel->setObjectName(QStringLiteral("trayPlaybackPanel"));
    panel->setFixedSize(156, 185);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(0);
    m_traySongLabel = new TraySongLabel(panel);
    m_traySongLabel->setTextFormat(Qt::PlainText);
    m_traySongLabel->setFixedSize(136, 26);
    m_traySongLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_traySongLabel);
    layout->addSpacing(6);
    auto *controls = new QHBoxLayout;
    controls->setSpacing(14);
    m_trayPreviousButton = new QPushButton(panel);
    m_trayPlayButton = new QPushButton(panel);
    m_trayNextButton = new QPushButton(panel);
    for (QPushButton *button : {m_trayPreviousButton, m_trayPlayButton, m_trayNextButton}) {
        button->setFixedSize(32, 32);
        button->setIconSize(QSize(20, 20));
        button->setCursor(Qt::PointingHandCursor);
        button->setAutoDefault(false);
        controls->addWidget(button);
    }
    m_trayPlayButton->setObjectName(QStringLiteral("trayPrimaryPlay"));
    m_trayPlayButton->setFixedSize(44, 44);
    m_trayPlayButton->setIconSize(QSize(22, 22));
    m_trayPreviousButton->setIcon(tintedIcon(QStringLiteral(":/icons/previous.svg"), QSize(20, 20),
                                             Qt::black, devicePixelRatioF()));
    m_trayPreviousButton->setToolTip(QStringLiteral("上一曲"));
    m_trayPreviousButton->setAccessibleName(QStringLiteral("上一曲"));
    m_trayNextButton->setIcon(tintedIcon(QStringLiteral(":/icons/next.svg"), QSize(20, 20),
                                         Qt::black, devicePixelRatioF()));
    m_trayNextButton->setToolTip(QStringLiteral("下一曲"));
    m_trayNextButton->setAccessibleName(QStringLiteral("下一曲"));
    layout->addLayout(controls);
    layout->addSpacing(6);

    auto *options = new QHBoxLayout;
    options->setSpacing(4);
    m_trayModeButton = new QPushButton(panel);
    m_trayModeButton->setObjectName(QStringLiteral("trayMode"));
    m_trayModeButton->setCursor(Qt::PointingHandCursor);
    m_trayModeButton->setFixedSize(100, 32);
    m_trayModeButton->setIconSize(QSize(20, 20));
    m_trayModeButton->setAutoDefault(false);
    options->addWidget(m_trayModeButton);
    m_trayDesktopLyricsButton = new QPushButton(QStringLiteral("桌"), panel);
    m_trayDesktopLyricsButton->setObjectName(QStringLiteral("trayDesktopLyrics"));
    m_trayDesktopLyricsButton->setFixedSize(32, 32);
    m_trayDesktopLyricsButton->setCursor(Qt::PointingHandCursor);
    m_trayDesktopLyricsButton->setAutoDefault(false);
    m_trayDesktopLyricsButton->setCheckable(true);
    options->addWidget(m_trayDesktopLyricsButton);
    layout->addLayout(options);
    layout->addSpacing(14);

    auto *separator = new QWidget(panel);
    separator->setFixedHeight(1);
    separator->setStyleSheet(QStringLiteral("background: rgba(90, 65, 110, 36);"));
    layout->addWidget(separator);
    layout->addSpacing(6);
    auto *exitButton = new QPushButton(QStringLiteral("退出"), panel);
    exitButton->setObjectName(QStringLiteral("trayExit"));
    exitButton->setCursor(Qt::PointingHandCursor);
    // 左内边距包含上一曲按钮的 6px 居中留白和 SVG 图形自身的 3px 留白。
    exitButton->setFixedHeight(30);
    exitButton->setAutoDefault(false);
    layout->addWidget(exitButton);
    auto *panelAction = new QWidgetAction(menu);
    panelAction->setDefaultWidget(panel);
    menu->addAction(panelAction);
    // 托盘字号固定，避免设置页应用界面字体时统一覆盖。
    QFont trayFont(QStringLiteral("KaiTi"));
    trayFont.setPixelSize(15);
    menu->setFont(trayFont);
    menu->setProperty("fixedTrayFont", true);
    const auto trayWidgets = menu->findChildren<QWidget *>();
    for (QWidget *widget : trayWidgets) {
        widget->setFont(trayFont);
        widget->setProperty("fixedTrayFont", true);
    }

    connect(m_trayPreviousButton, &QPushButton::clicked, this, &MainWindow::playPrevious);
    connect(m_trayPlayButton, &QPushButton::clicked, this, &MainWindow::togglePlayback);
    connect(m_trayNextButton, &QPushButton::clicked, this, [this] {
        playNext();
    });
    connect(m_trayModeButton, &QPushButton::clicked, this, &MainWindow::cyclePlayMode);
    connect(m_trayDesktopLyricsButton, &QPushButton::clicked, this, &MainWindow::toggleDesktopLyrics);
    connect(menu, &QMenu::aboutToShow, this, &MainWindow::updateTrayPlayback);
    connect(exitButton, &QPushButton::clicked, this, [this, menu] {
        // 显式退出绕过关闭到托盘，随后由析构函数释放播放器及音效线程。
        m_exitRequested = true;
        menu->hide();
        QApplication::quit();
    });
    m_trayIcon->setContextMenu(menu);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::DoubleClick) {
                    restoreFromTray();
                }
            });
    updateTrayPlayback();
    m_trayIcon->show();
}

void MainWindow::updateTrayPlayback()
{
    if (m_player == nullptr) {
        return;
    }
    const QUrl source = m_player->source();
    const bool hasSource = !source.isEmpty();
    const bool hasSongs = !m_playbackController->queue().isEmpty() || !m_songs.isEmpty();
    const bool playing = m_playbackController != nullptr && m_playbackController->isPlaying();
    const QString songName =
        hasSource ? QFileInfo(source.isLocalFile() ? source.toLocalFile() : source.path())
                        .completeBaseName()
                  : QStringLiteral("暂无播放歌曲");
    // 原生任务栏预览使用窗口标题；从实际音源取名，避免列表筛选改变显示目标。
    const QString title =
        hasSource ? QStringLiteral("%1 — Whq Music").arg(songName) : QStringLiteral("Whq Music");
    if (windowTitle() != title) {
        setWindowTitle(title);
    }
    if (m_trayIcon == nullptr || m_traySongLabel == nullptr) {
        return;
    }
    m_trayIcon->setToolTip(songName.left(120));
    static_cast<TraySongLabel *>(m_traySongLabel)->setSongName(songName);
    m_traySongLabel->setToolTip(songName);
    m_traySongLabel->setAccessibleName(songName);
    m_trayPlayButton->setIcon(tintedIcon(playing ? QStringLiteral(":/icons/pause.svg")
                                                 : QStringLiteral(":/icons/play.svg"),
                                         QSize(22, 22), Qt::black, devicePixelRatioF()));
    m_trayPlayButton->setToolTip(playing ? QStringLiteral("暂停") : QStringLiteral("播放"));
    m_trayPlayButton->setAccessibleName(m_trayPlayButton->toolTip());
    const QStringList modeIcons = {
        QStringLiteral(":/icons/repeat_off.svg"), QStringLiteral(":/icons/repeat_all.svg"),
        QStringLiteral(":/icons/repeat_one.svg"), QStringLiteral(":/icons/shuffle.svg")};
    const int mode = qBound(0, static_cast<int>(m_playMode), 3);
    m_trayModeButton->setIcon(
        tintedIcon(modeIcons.at(mode), QSize(20, 20), Qt::black, devicePixelRatioF()));
    m_trayModeButton->setText(QStringLiteral("\u2009%1").arg(m_modeButton->toolTip()));
    m_trayModeButton->setToolTip(
        QStringLiteral("播放模式：%1（点击切换）").arg(m_modeButton->toolTip()));
    m_trayModeButton->setAccessibleName(m_trayModeButton->toolTip());
    const bool controlsEnabled =
        isEnabled() && !m_exitRequested && !m_libraryImportInProgress && !m_fileEditInProgress;
    m_trayPreviousButton->setEnabled(controlsEnabled && (hasSongs || hasSource));
    m_trayPlayButton->setEnabled(controlsEnabled && (hasSongs || hasSource));
    m_trayNextButton->setEnabled(controlsEnabled && hasSongs);
    m_trayModeButton->setEnabled(controlsEnabled);
    const bool lyricsVisible = m_desktopLyrics != nullptr && m_desktopLyrics->isVisible();
    m_trayDesktopLyricsButton->setChecked(lyricsVisible);
    m_trayDesktopLyricsButton->setToolTip(
        lyricsVisible ? QStringLiteral("隐藏桌面歌词") : QStringLiteral("显示桌面歌词"));
    m_trayDesktopLyricsButton->setAccessibleName(m_trayDesktopLyricsButton->toolTip());
    m_trayDesktopLyricsButton->setEnabled(controlsEnabled && m_desktopLyrics != nullptr);
}

void MainWindow::restoreFromTray()
{
    // 去掉最小化状态但保留最大化状态，恢复用户隐藏前的窗口布局。
    setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
}
