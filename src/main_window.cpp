#include "main_window.h"
#include "player_widgets.h"
#include "music_library_repository.h"
#include "playback_controller.h"
#include "metadata_scanner.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QEasingCurve>
#include <QEvent>
#include <QFileDialog>
#include <QFontComboBox>
#include <QImage>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QSpinBox>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolTip>
#include <QVariant>

#ifdef Q_OS_WIN
#include <shobjidl.h>
#include <windows.h>

#endif

using namespace PlayerWidgets;

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
#ifdef Q_OS_WIN
    // 必须在窗口首次显示前注册消息，收到系统通知后才能添加缩略图按钮。
    m_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarButtonCreated");
    if (m_taskbarCreatedMessage == 0) {
        qWarning() << "Failed to register taskbar creation message" << GetLastError();
    }
#endif
    setWindowFlag(Qt::FramelessWindowHint);
    setWindowTitle(QStringLiteral("Whq Music"));
    resize(1100, 700);
    buildUi();
    if (!initializeDatabase()) {
        centralWidget()->setEnabled(false);
        QTimer::singleShot(0, qApp, [] {
            QCoreApplication::exit(1);
        });
        return;
    }
    const QString applicationFont =
        setting(QStringLiteral("applicationFont"), QStringLiteral("KaiTi"));
    const QString lyricsFont = setting(QStringLiteral("lyricsFont"), QStringLiteral("KaiTi"));
    const int applicationFontSize = qBound(
        10, setting(QStringLiteral("applicationFontSize"), QStringLiteral("16")).toInt(), 24);
    const int lyricsFontSize =
        qBound(12, setting(QStringLiteral("lyricsFontSize"), QStringLiteral("24")).toInt(), 36);
    applyFonts(applicationFont, applicationFontSize, lyricsFont, lyricsFontSize);
    m_applicationFontSelector->setCurrentFont(QFont(applicationFont));
    m_lyricsFontSelector->setCurrentFont(QFont(lyricsFont));
    m_applicationFontSizeSelector->setValue(applicationFontSize);
    m_lyricsFontSizeSelector->setValue(lyricsFontSize);
    m_showSimplifiedLyrics =
        setting(QStringLiteral("simplifiedLyrics"), QStringLiteral("1")) != QStringLiteral("0");
    updateLyricsScriptButton();
    loadGroups();

    const int volume = setting(QStringLiteral("volume"), QStringLiteral("70")).toInt();
    m_volume->setValue(volume);
    m_audioEffectProcessor->setVolume(volume / 100.0);
    m_playMode =
        static_cast<PlayMode>(setting(QStringLiteral("playMode"), QStringLiteral("1")).toInt());
    updatePlayModeButton();
    applyAudioEffect(static_cast<AudioEffectProcessor::Preset>(
        setting(QStringLiteral("audioEffect"), QStringLiteral("0")).toInt()));
    QString background = setting(QStringLiteral("background"), kDefaultBackgroundPath);
    // 未设置背景或自定义图片失效时，使用随程序内置的默认图片。
    auto *surface = static_cast<AppSurface *>(centralWidget());
    if (background.isEmpty() || !surface->setBackground(background)) {
        background = kDefaultBackgroundPath;
        surface->setBackground(background);
    }
    m_backgroundPath = background;
    m_backgroundEditor->setText(background);
    updateLyricsPalette(background);
    updateLyricsScriptButtonPalette(background);
    updateCaptionButtonPalette(background);
    updateSongInfoPalette(background);
    updateDialogTheme(background);
    initializeSystemTray();
}

MainWindow::~MainWindow()
{
    if (m_metadataReadFuture.isRunning()) {
        m_metadataReadFuture.cancel();
        m_metadataReadFuture.waitForFinished();
    }
    // 提交阶段涉及文件与数据库一致性，退出时等后台操作结束后再销毁依赖。
    if (m_editFuture.isRunning()) {
        m_editFuture.waitForFinished();
    }
    if (m_refreshFuture.isRunning()) {
        m_refreshFuture.cancel();
        m_refreshFuture.waitForFinished();
    }
    m_player->disconnect(this);
    delete m_playbackController;
    m_playbackController = nullptr;
    m_metadataScanner->suspend();
    if (m_importFuture.isRunning()) {
        m_importFuture.cancel();
        m_importFuture.waitForFinished();
    }
    if (m_volumeSaveTimer != nullptr && m_volumeSaveTimer->isActive()) {
        saveSetting(QStringLiteral("volume"), QString::number(m_volume->value()), false);
    }
    if (m_trayIcon != nullptr) {
        m_trayIcon->hide();
    }
    if (m_player != nullptr) {
        // 先等待解码生产者停止，再停止输出反馈线程，最后销毁共享接收方。
        m_player->disconnect(this);
        m_player->stop();
    }
    if (m_audioEffectThread != nullptr) {
        m_audioEffectThread->quit();
        m_audioEffectThread->wait();
        m_audioEffectProcessor = nullptr;
    }
    delete m_player;
    m_player = nullptr;
#ifdef Q_OS_WIN
    if (m_taskbar != nullptr) {
        m_taskbar->Release();
        m_taskbar = nullptr;
    }
    if (m_taskbarComInitialized) {
        CoUninitialize();
    }
#endif
}

void MainWindow::chooseBackground()
{
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择背景"), {}, QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp)"));
    if (!file.isEmpty()) {
        if (!saveSetting(QStringLiteral("background"), file)) {
            return;
        }
        applyBackground(file);
    }
}

void MainWindow::applyBackground(const QString &path)
{
    m_backgroundPath = path;
    const quint64 generation = ++m_backgroundUpdateGeneration;
    using BackgroundImages = QPair<QImage, QImage>;
    auto *watcher = new QFutureWatcher<BackgroundImages>(this);
    connect(watcher, &QFutureWatcher<BackgroundImages>::finished, this,
            [this, watcher, path, generation] {
                const auto images = watcher->result();
                watcher->deleteLater();
                if (generation != m_backgroundUpdateGeneration) {
                    return;
                }
                if (images.first.isNull()) {
                    qWarning() << "Failed to load selected background";
                    return;
                }
                cacheBackgroundSample(path, images.first);
                static_cast<AppSurface *>(centralWidget())->setBackground(path);
                updateLyricsPalette(path);
                updateLyricsScriptButtonPalette(path);
                updateCaptionButtonPalette(path);
                updateSongInfoPalette(path);
                updateDialogTheme(path);
#ifdef Q_OS_WIN
                m_taskbarBackground = images.second;
                m_taskbarBackgroundPath = path;
#endif
                updateTaskbarBackground();
            });
    // 解码和采样在后台完成；主线程只安装结果，持续处理音频数据和界面事件。
    watcher->setFuture(QtConcurrent::run([path] {
        const QImage preview = loadScaledImage(path, QSize(2048, 2048));
        return BackgroundImages{
            preview.scaled(QSize(384, 384), Qt::KeepAspectRatio, Qt::SmoothTransformation), preview};
    }));
}

void MainWindow::showSettings()
{
    if (m_contentStack->currentIndex() == 2) {
        m_contentStack->setCurrentIndex(m_settingsReturnIndex);
    } else {
        m_settingsReturnIndex = m_contentStack->currentIndex();
        m_contentStack->setCurrentIndex(2);
    }
}

void MainWindow::applyFonts(const QString &applicationFont, int applicationFontSize,
                            const QString &lyricsFont, int lyricsFontSize)
{
    if (m_baseStyleSheet.isEmpty()) {
        m_baseStyleSheet = qApp->styleSheet();
    }
    QFont interfaceFont = QApplication::font();
    interfaceFont.setFamily(applicationFont);
    interfaceFont.setPixelSize(applicationFontSize);
    QApplication::setFont(interfaceFont);
    qApp->setStyleSheet(
        styleSheetWithFontSizes(m_baseStyleSheet, applicationFontSize, lyricsFontSize) +
        m_dialogThemeStyle);

    QFont toolTipFont = interfaceFont;
    toolTipFont.setPixelSize(qMax(8, applicationFontSize - 2));
    QToolTip::setFont(toolTipFont);
    const auto widgets = QApplication::allWidgets();
    for (QWidget *widget : widgets) {
        if (widget == m_lyrics || m_lyrics->isAncestorOf(widget)) {
            continue;
        }
        // 设置预览使用各自选择的字体，不参与界面字体的统一替换。
        if (widget->objectName() == QStringLiteral("settingsPreview") ||
            widget->property("fixedTrayFont").toBool()) {
            continue;
        }

        QFont widgetFont = widget->font();
        widgetFont.setFamily(applicationFont);
        widget->setFont(widgetFont);
    }

    QFont lyricDisplayFont = m_lyrics->font();
    lyricDisplayFont.setFamily(lyricsFont);
    lyricDisplayFont.setPixelSize(lyricsFontSize);
    m_lyrics->setFont(lyricDisplayFont);
}

void MainWindow::toggleMaximized()
{
    isMaximized() ? showNormal() : showMaximized();
    updateMaximizeButton();
}

void MainWindow::updateMaximizeButton()
{
    if (m_maximizeButton == nullptr) {
        return;
    }
    configureIconButton(m_maximizeButton,
                        isMaximized() ? QStringLiteral(":/icons/restore.svg")
                                      : QStringLiteral(":/icons/maximize.svg"),
                        isMaximized() ? QStringLiteral("还原") : QStringLiteral("最大化"),
                        QSize(22, 22));
    updateCaptionButtonPalette(m_backgroundPath);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (m_lyricsScrollTimer != nullptr &&
        (watched == this || watched == m_lyrics || watched == m_lyrics->viewport())) {
        if (event->type() == QEvent::Hide) {
            m_lyricsScrollTimer->stop();
            m_lyricsViewDirty = true;
        } else if (event->type() == QEvent::Show || event->type() == QEvent::Resize ||
                   event->type() == QEvent::FontChange ||
                   event->type() == QEvent::WindowStateChange) {
            m_lyricsViewDirty = true;
            // Wait for the visibility/layout change to finish before measuring the active line.
            m_lyricsScrollTimer->start(0);
        }
    }
    if (m_queueDrawerOpen && m_queueDrawer != nullptr && m_queueButton != nullptr &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseButtonDblClick)) {
        auto *clickedWidget = qobject_cast<QWidget *>(watched);
        if (clickedWidget != nullptr) {
            const bool insideDrawer =
                clickedWidget == m_queueDrawer || m_queueDrawer->isAncestorOf(clickedWidget);
            // 切换按钮由 clicked 信号处理，避免按下时关闭、释放时又重新打开。
            const bool onToggleButton =
                clickedWidget == m_queueButton || m_queueButton->isAncestorOf(clickedWidget);
            if (!insideDrawer && !onToggleButton) {
                toggleQueueDrawer();
            }
        }
    }
    // 不拦截原始事件，外部按钮、输入框等仍正常响应本次点击。
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_exitRequested || qApp->isSavingSession()) {
        event->accept();
        return;
    }
    event->ignore();
    if (m_trayIcon != nullptr && QSystemTrayIcon::isSystemTrayAvailable()) {
        m_trayIcon->show();
        hide();
    } else {
        // 托盘暂不可用时保留任务栏入口，避免程序隐藏后无法找回。
        showMinimized();
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        updateMaximizeButton();
    }
    if (event->type() == QEvent::EnabledChange || event->type() == QEvent::WindowStateChange) {
        updateTaskbarToolbar();
    }
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (m_lyricsButtonPaletteTimer != nullptr) {
        m_lyricsButtonPaletteTimer->start();
    }
    updateQueueDrawerGeometry();
}

QRect MainWindow::queueDrawerRect(bool open) const
{
    if (centralWidget() == nullptr || m_contentStack == nullptr) {
        return {};
    }
    const int availableWidth = centralWidget()->width();
    const int drawerWidth = qMin(availableWidth, qBound(340, qRound(availableWidth * 0.32), 460));
    const int drawerX = open ? availableWidth - drawerWidth : availableWidth;
    return QRect(drawerX, m_contentStack->y(), drawerWidth, m_contentStack->height());
}

void MainWindow::updateQueueDrawerGeometry()
{
    if (m_queueDrawer == nullptr || centralWidget() == nullptr) {
        return;
    }
    if (m_queueDrawerAnimation != nullptr &&
        m_queueDrawerAnimation->state() == QAbstractAnimation::Running) {
        m_queueDrawerAnimation->stop();
    }
    m_queueDrawer->setGeometry(queueDrawerRect(m_queueDrawerOpen));
    m_queueDrawer->update();
    if (m_queueDrawerOpen) {
        m_queueDrawer->raise();
    }
}

void MainWindow::toggleQueueDrawer()
{
    if (m_queueDrawer == nullptr || m_queueDrawerAnimation == nullptr) {
        return;
    }
    const bool opening = !m_queueDrawerOpen;
    m_queueDrawerAnimation->stop();
    const QRect currentGeometry =
        m_queueDrawer->isVisible() ? m_queueDrawer->geometry() : queueDrawerRect(false);
    m_queueDrawerOpen = opening;
    if (!opening) {
        m_queueSearchEditor->clear();
    }
    m_queueDrawer->setGeometry(currentGeometry);
    m_queueDrawer->update();
    m_queueDrawer->show();
    m_queueDrawer->raise();
    m_queueDrawerAnimation->setEasingCurve(opening ? QEasingCurve::OutCubic
                                                   : QEasingCurve::InCubic);
    m_queueDrawerAnimation->setStartValue(currentGeometry);
    m_queueDrawerAnimation->setEndValue(queueDrawerRect(opening));
    m_queueDrawerAnimation->start();
}

qint64 MainWindow::currentGroupId() const
{
    return m_activeGroupId;
}

qint64 MainWindow::currentSongId() const
{
    const QModelIndexList rows = m_songsTable->selectionModel()->selectedRows();
    return rows.isEmpty() ? 0 : rows.first().data(Qt::UserRole).toLongLong();
}

QString MainWindow::setting(const QString &key, const QString &fallback) const
{
    return m_repository->setting(key, fallback);
}

bool MainWindow::saveSetting(const QString &key, const QString &value, bool showError)
{
    QString error;
    if (m_repository->saveSettings({{key, value}}, error)) {
        return true;
    }
    qWarning() << "Failed to save setting:" << key;
    if (showError) {
        auto *dialog = new QMessageBox(QMessageBox::Warning, QStringLiteral("保存失败"), error,
                                       QMessageBox::Ok, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    }
    return false;
}

bool MainWindow::saveSettings(const QList<QPair<QString, QString>> &values)
{
    QString error;
    if (m_repository->saveSettings(values, error)) {
        return true;
    }
    QMessageBox::warning(this, QStringLiteral("保存失败"), error);
    return false;
}

QString MainWindow::formatTime(qint64 milliseconds)
{
    const qint64 seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2")
        .arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
