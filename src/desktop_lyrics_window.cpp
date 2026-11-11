#include "desktop_lyrics_window.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QEnterEvent>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QGridLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QTextBoundaryFinder>
#include <QTimer>
#include <QTransform>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QtMath>

#include <array>

namespace {

constexpr int kEdgeWidth = 9;
constexpr int kMinimumVisibleExtent = 48;
constexpr int kStaggerCharacters = 2;
const QSize kHorizontalMinimumSize(240, 130);
const QSize kVerticalMinimumSize(130, 240);

struct ColorPreset
{
    QString name;
    QColor base;
    QColor played;
};

const std::array<ColorPreset, 16> &colorPresets()
{
    static const std::array<ColorPreset, 16> presets{{
        {QStringLiteral("雪白"), QColor("#edf0f7"), QColor("#278bff")},
        {QStringLiteral("银灰"), QColor("#b8c0cc"), QColor("#ff755b")},
        {QStringLiteral("深灰"), QColor("#303746"), QColor("#ffd76e")},
        {QStringLiteral("墨黑"), QColor("#171b25"), QColor("#b783ff")},
        {QStringLiteral("天蓝"), QColor("#76dcff"), QColor("#ff83cf")},
        {QStringLiteral("海蓝"), QColor("#5198ff"), QColor("#ffe082")},
        {QStringLiteral("靛蓝"), QColor("#6374dc"), QColor("#83efc6")},
        {QStringLiteral("淡紫"), QColor("#b8a4ff"), QColor("#65e7b5")},
        {QStringLiteral("紫罗兰"), QColor("#aa6be8"), QColor("#ffdd75")},
        {QStringLiteral("樱粉"), QColor("#ffb3cf"), QColor("#429bff")},
        {QStringLiteral("玫红"), QColor("#ef6caa"), QColor("#76e9ff")},
        {QStringLiteral("珊瑚"), QColor("#ff8d82"), QColor("#71dfff")},
        {QStringLiteral("橙色"), QColor("#ffb15c"), QColor("#9382ff")},
        {QStringLiteral("金黄"), QColor("#ffdb70"), QColor("#54a1ff")},
        {QStringLiteral("薄荷"), QColor("#83efc6"), QColor("#fa85c7")},
        {QStringLiteral("青绿"), QColor("#4dd6b4"), QColor("#c98bff")}
    }};
    return presets;
}

bool isLatinCharacter(QChar character)
{
    return character.unicode() < 128 &&
           (character.isLetterOrNumber() || character == QLatin1Char('\'') ||
            character == QLatin1Char('-'));
}

} // namespace

DesktopLyricsWindow::DesktopLyricsWindow()
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowDoesNotAcceptFocus)
{
    setObjectName(QStringLiteral("desktopLyricsWindow"));
    setWindowTitle(QStringLiteral("桌面歌词"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setMouseTracking(true);
    setMinimumSize(kHorizontalMinimumSize);
    setStyleSheet(QStringLiteral(
        "#desktopLyricsToolbar { background: transparent; }"
        "#desktopLyricsToolbar QPushButton { min-width: 0; min-height: 0;"
        " padding: 0; color: #f5f3ff;"
        " background: transparent; border: none;"
        " border-radius: 6px; font-size: 14px; }"
        "#desktopLyricsToolbar QPushButton:hover { background: rgba(255, 255, 255, 35); }"
        "#desktopLyricsToolbar QPushButton:pressed { background: rgba(255, 255, 255, 55); }"
        "#desktopLyricsToolbar QPushButton:disabled { background: transparent; }"
        "#desktopLyricsToolbar QPushButton#desktopLyricsClose { font-size: 22px; }"
        "#desktopLyricsToolbar QPushButton#desktopLyricsClose:hover { background: #c42b3b; }"
        "#desktopLyricsToolbar QPushButton#desktopLyricsClose:pressed { background: #a92331; }"));

    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("desktopLyricsToolbar"));
    m_playbackControls = new QWidget(m_toolbar);
    auto *toolbarLayout = new QGridLayout(m_playbackControls);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(4);
    const auto addButton = [this, toolbarLayout](const QString &text, const QString &tip) {
        auto *button = new QPushButton(text, m_playbackControls);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setFixedSize(32, 28);
        button->setIconSize(QSize(20, 20));
        toolbarLayout->addWidget(button, 0, toolbarLayout->count());
        return button;
    };
    m_previousButton = addButton(QString(), QStringLiteral("上一曲"));
    m_previousButton->setIcon(QIcon(QStringLiteral(":/icons/previous.svg")));
    m_playButton = addButton(QString(), QStringLiteral("播放"));
    m_playButton->setIcon(QIcon(QStringLiteral(":/icons/play.svg")));
    m_nextButton = addButton(QString(), QStringLiteral("下一曲"));
    m_nextButton->setIcon(QIcon(QStringLiteral(":/icons/next.svg")));
    m_modeButton = addButton(QString(), QStringLiteral("播放模式"));
    auto *colors = addButton(QString(), QStringLiteral("桌面歌词颜色"));
    colors->setIcon(QIcon(QStringLiteral(":/icons/font_color.svg")));
    connect(m_previousButton, &QPushButton::clicked, this, &DesktopLyricsWindow::previousRequested);
    connect(m_playButton, &QPushButton::clicked, this, &DesktopLyricsWindow::playPauseRequested);
    connect(m_nextButton, &QPushButton::clicked, this, &DesktopLyricsWindow::nextRequested);
    connect(m_modeButton, &QPushButton::clicked, this, &DesktopLyricsWindow::playModeRequested);
    connect(colors, &QPushButton::clicked, this, [this, colors] {
        showColorMenu(colors);
    });
    m_closeButton = new QPushButton(QStringLiteral("×"), m_toolbar);
    m_closeButton->setObjectName(QStringLiteral("desktopLyricsClose"));
    m_closeButton->setToolTip(QStringLiteral("关闭桌面歌词"));
    m_closeButton->setAccessibleName(QStringLiteral("关闭桌面歌词"));
    m_closeButton->setFocusPolicy(Qt::NoFocus);
    m_closeButton->setCursor(Qt::PointingHandCursor);
    m_closeButton->setFixedHeight(28);
    connect(m_closeButton, &QPushButton::clicked, this, &QWidget::close);
    m_playbackControls->adjustSize();
    layoutToolbar();
    m_toolbar->hide();

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(300);
    connect(m_saveTimer, &QTimer::timeout, this, [this] {
        emit configurationChanged(configuration());
    });
    resize(680, 130);
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] {
        keepOnScreen();
        scheduleSave();
    });
    const auto watchScreen = [this](QScreen *screen) {
        connect(screen, &QScreen::availableGeometryChanged, this, [this] {
            keepOnScreen();
            scheduleSave();
        });
    };
    for (QScreen *screen : QGuiApplication::screens()) {
        watchScreen(screen);
    }
    connect(qApp, &QGuiApplication::screenAdded, this, watchScreen);
}

void DesktopLyricsWindow::restoreConfiguration(const QString &configuration,
                                               const QPoint &preferredScreen)
{
    m_restoring = true;
    m_saveTimer->stop();
    const QJsonObject root = QJsonDocument::fromJson(configuration.toUtf8()).object();
    QScreen *screen = QGuiApplication::screenAt(preferredScreen);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect area = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
    const QSize defaultSize(qMin(680, area.width()), qMin(130, area.height()));
    const int defaultX = area.center().x() - defaultSize.width() / 2;
    QRect saved(QPoint(root.value(QStringLiteral("x")).toInt(defaultX),
                       root.value(QStringLiteral("y")).toInt(area.top() + 12)),
                QSize(root.value(QStringLiteral("width")).toInt(defaultSize.width()),
                      root.value(QStringLiteral("height")).toInt(defaultSize.height())));
    m_vertical = root.value(QStringLiteral("vertical")).toBool(false);
    m_colorIndex = -1;
    const QString savedColor = root.value(QStringLiteral("color")).toString();
    for (int index = 0; index < static_cast<int>(colorPresets().size()); ++index) {
        if (colorPresets().at(index).base.name() == savedColor) {
            m_colorIndex = index;
            break;
        }
    }
    // Restore the target geometry before enforcing its orientation-specific minimum.
    setMinimumSize(kHorizontalMinimumSize.boundedTo(kVerticalMinimumSize));
    setGeometry(saved);
    keepOnScreen();
    updateOrientation();
    m_saveTimer->stop();
    m_restoring = false;
    rebuildPaths();
}

QString DesktopLyricsWindow::configuration() const
{
    const QRect bounds = geometry();
    const QJsonObject root{{QStringLiteral("x"), bounds.x()},
                           {QStringLiteral("y"), bounds.y()},
                           {QStringLiteral("width"), bounds.width()},
                           {QStringLiteral("height"), bounds.height()},
                           {QStringLiteral("vertical"), m_vertical},
                           {QStringLiteral("color"), m_colorIndex >= 0
                                ? colorPresets().at(m_colorIndex).base.name() : QString()}};
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void DesktopLyricsWindow::setLyricsFont(const QFont &font)
{
    m_lyricsFont = font;
    rebuildPaths();
}

void DesktopLyricsWindow::showColorMenu(QPushButton *button)
{
    if (m_colorMenu == nullptr) {
        m_colorMenu = new QMenu(this);
        m_colorMenu->setStyleSheet(QStringLiteral(
            "QMenu { background: #1b1e2c; border: 1px solid #55516c; padding: 10px; }"
            "QWidget#desktopColorPanel { background: transparent; }"
            "QWidget#desktopColorPanel QLabel { color: #f5f3ff; background: transparent; }"
            "QWidget#desktopColorPanel QPushButton { min-width: 82px; min-height: 32px;"
            " padding: 3px 8px; border-radius: 6px; color: #f5f3ff;"
            " background: #2b2e40; border: 1px solid #45495d; }"
            "QWidget#desktopColorPanel QPushButton:hover { background: #41445b; }"
            "QWidget#desktopColorPanel QPushButton:checked { border-color: #b4aaff;"
            " background: #50476f; }"));
        auto *panel = new QWidget(m_colorMenu);
        panel->setObjectName(QStringLiteral("desktopColorPanel"));
        auto *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(10);
        auto *follow = new QPushButton(QStringLiteral("恢复默认配色"), panel);
        follow->setProperty("colorChoice", -1);
        follow->setCheckable(true);
        layout->addWidget(follow);
        connect(follow, &QPushButton::clicked, this, [this] {
            m_colorIndex = -1;
            update();
            scheduleSave();
            m_colorMenu->close();
        });
        auto *hint =
            new QLabel(QStringLiteral("选择未播放颜色 · 色块预览：未播放 → 播放"), panel);
        layout->addWidget(hint);
        auto *grid = new QGridLayout;
        grid->setSpacing(8);
        for (int index = 0; index < static_cast<int>(colorPresets().size()); ++index) {
            const ColorPreset &preset = colorPresets().at(index);
            QPixmap preview(42, 18);
            preview.fill(Qt::transparent);
            QPainter painter(&preview);
            painter.fillRect(QRect(0, 0, 21, 18), preset.base);
            painter.fillRect(QRect(21, 0, 21, 18), preset.played);
            painter.setPen(QColor(255, 255, 255, 90));
            painter.drawRect(preview.rect().adjusted(0, 0, -1, -1));
            painter.end();
            auto *choice = new QPushButton(QIcon(preview), preset.name, panel);
            choice->setIconSize(preview.size());
            choice->setProperty("colorChoice", index);
            choice->setCheckable(true);
            choice->setToolTip(QStringLiteral("%1：左侧为未播放颜色，右侧为播放填色。")
                                   .arg(preset.name));
            grid->addWidget(choice, index / 4, index % 4);
            connect(choice, &QPushButton::clicked, this, [this, index] {
                m_colorIndex = index;
                update();
                scheduleSave();
                m_colorMenu->close();
            });
        }
        layout->addLayout(grid);
        auto *action = new QWidgetAction(m_colorMenu);
        action->setDefaultWidget(panel);
        m_colorMenu->addAction(action);
    }
    const auto choices = m_colorMenu->findChildren<QPushButton *>();
    for (QPushButton *choice : choices) {
        choice->setChecked(choice->property("colorChoice").toInt() == m_colorIndex);
    }
    m_colorMenu->exec(button->mapToGlobal(QPoint(0, button->height() + 6)));
    m_hovered = rect().contains(mapFromGlobal(QCursor::pos()));
    m_toolbar->setVisible(m_hovered);
    update();
}

void DesktopLyricsWindow::setPlaybackControls(const QIcon &playIcon, const QString &playTip,
                                              const QIcon &modeIcon, const QString &modeTip,
                                              bool enabled)
{
    if (m_playButton->icon().cacheKey() != playIcon.cacheKey()) {
        m_playButton->setIcon(playIcon);
    }
    if (m_modeButton->icon().cacheKey() != modeIcon.cacheKey()) {
        m_modeButton->setIcon(modeIcon);
    }
    m_playButton->setToolTip(playTip);
    m_playButton->setAccessibleName(playTip);
    m_modeButton->setToolTip(modeTip);
    m_modeButton->setAccessibleName(modeTip);
    for (QPushButton *button : {m_previousButton, m_playButton, m_nextButton, m_modeButton}) {
        button->setEnabled(enabled);
    }
}

void DesktopLyricsWindow::layoutToolbar()
{
    const auto buttons = m_playbackControls->findChildren<QPushButton *>(
        QString(), Qt::FindDirectChildrenOnly);
    const int columns = qBound(1, (width() - 16 + 4) / 36, static_cast<int>(buttons.size()));
    const int rows = (static_cast<int>(buttons.size()) + columns - 1) / columns;
    const int controlsWidth = columns * 32 + (columns - 1) * 4;
    const int controlsHeight = rows * 28 + (rows - 1) * 4;
    // Rebuild only the toolbar layout on resize so narrow vertical windows can wrap controls.
    delete m_playbackControls->layout();
    auto *toolbarLayout = new QGridLayout(m_playbackControls);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(4);
    for (int index = 0; index < buttons.size(); ++index) {
        toolbarLayout->addWidget(buttons.at(index), index / columns, index % columns);
    }
    const int closeWidth = 32;
    const bool narrow = width() < controlsWidth + 2 * (closeWidth + 16);
    const int controlsTop = narrow ? 32 : 0;
    m_toolbarHeight = controlsTop + controlsHeight + 12;
    m_toolbar->setGeometry(8, 6, width() - 16, m_toolbarHeight - 6);
    m_playbackControls->setGeometry((m_toolbar->width() - controlsWidth) / 2, controlsTop,
                                    controlsWidth, controlsHeight);
    m_closeButton->setGeometry(m_toolbar->width() - closeWidth, 0, closeWidth, 28);
}

QRectF DesktopLyricsWindow::lyricsArea() const
{
    return QRectF(16, m_toolbarHeight + 4, width() - 32, height() - m_toolbarHeight - 12);
}

QRectF DesktopLyricsWindow::lineArea(int slot) const
{
    const QRectF area = lyricsArea();
    // Slot zero is always the upper row or right column, independent of playback.
    if (m_vertical) {
        const qreal columnGap = qMax(12.0, m_lineHeight * 0.3);
        const qreal center = area.center().x() + (slot == 0 ? 0.5 : -0.5) *
                                                   (m_lineHeight + columnGap);
        const qreal indent = slot * kStaggerCharacters * m_characterHeight;
        return QRectF(center - m_lineHeight / 2.0, area.top() + indent,
                      m_lineHeight, qMax(0.0, area.height() - indent));
    }
    const qreal lineGap = qMax(6.0, m_lineHeight * 0.3);
    const qreal rowHeight = qMax(0.0, (area.height() - lineGap) / 2.0);
    const qreal indent = slot * kStaggerCharacters * m_characterWidth;
    return QRectF(area.left() + indent, area.top() + slot * (rowHeight + lineGap),
                  qMax(0.0, area.width() - indent), rowHeight);
}

void DesktopLyricsWindow::setLines(const Lyrics::Line &current, const QString &next, qreal progress,
                                   qreal scrollProgress, int currentSlot)
{
    const bool textChanged = m_current.text != current.text || m_next != next;
    const int boundedSlot = qBound(0, currentSlot, 1);
    const bool slotChanged = m_currentSlot != boundedSlot;
    const qreal boundedProgress = qBound(0.0, progress, 1.0);
    const qreal boundedScrollProgress = qBound(0.0, scrollProgress, 1.0);
    const bool progressChanged = !qFuzzyCompare(m_progress + 1.0, boundedProgress + 1.0);
    const QRectF bounds = m_currentPath.boundingRect();
    const QRectF area = lineArea(boundedSlot);
    const qreal available = m_vertical ? area.height() : area.width();
    const bool scrollChanged =
        (m_vertical ? bounds.height() : bounds.width()) > available &&
        !qFuzzyCompare(m_scrollProgress + 1.0, boundedScrollProgress + 1.0);
    m_current = current;
    m_next = next;
    m_currentSlot = boundedSlot;
    m_progress = boundedProgress;
    m_scrollProgress = boundedScrollProgress;
    if (textChanged) {
        rebuildPaths();
    } else if (isVisible() && (slotChanged || progressChanged || scrollChanged)) {
        update(lyricsArea().toAlignedRect());
    }
}

void DesktopLyricsWindow::showLyrics()
{
    keepOnScreen();
    show();
    emit visibilityChanged(true);
}

void DesktopLyricsWindow::keepOnScreen()
{
    // Keep partially off-screen and cross-monitor positions as long as a draggable patch remains.
    for (QScreen *candidate : QGuiApplication::screens()) {
        const QRect area = candidate->availableGeometry();
        const QRect intersection = geometry().intersected(area);
        const int visibleWidth = qMin(kMinimumVisibleExtent, qMin(width(), area.width()));
        const int visibleHeight = qMin(kMinimumVisibleExtent, qMin(height(), area.height()));
        if (intersection.width() >= visibleWidth && intersection.height() >= visibleHeight) {
            return;
        }
    }
    QScreen *screen = QGuiApplication::screenAt(geometry().center());
    if (screen == nullptr) {
        qint64 largestIntersection = 0;
        for (QScreen *candidate : QGuiApplication::screens()) {
            const QRect intersection = geometry().intersected(candidate->availableGeometry());
            const qint64 area = qint64(intersection.width()) * intersection.height();
            if (area > largestIntersection) {
                largestIntersection = area;
                screen = candidate;
            }
        }
    }
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }
    const QRect area = screen->availableGeometry();
    const int visibleWidth = qMin(kMinimumVisibleExtent, qMin(width(), area.width()));
    const int visibleHeight = qMin(kMinimumVisibleExtent, qMin(height(), area.height()));
    QRect bounds = geometry();
    bounds.moveLeft(qBound(area.left() - bounds.width() + visibleWidth, bounds.left(),
                          area.right() - visibleWidth + 1));
    bounds.moveTop(qBound(area.top() - bounds.height() + visibleHeight, bounds.top(),
                         area.bottom() - visibleHeight + 1));
    if (geometry() != bounds) {
        setGeometry(bounds);
    }
}

void DesktopLyricsWindow::scheduleSave()
{
    if (!m_restoring && m_saveTimer != nullptr) {
        m_saveTimer->start();
    }
}

void DesktopLyricsWindow::flushConfiguration()
{
    if (m_saveTimer->isActive()) {
        m_saveTimer->stop();
        emit configurationChanged(configuration());
    }
}

void DesktopLyricsWindow::updateOrientation()
{
    // Keep the previous direction near a square to prevent flicker while resizing.
    if (height() > width() * 1.15) {
        m_vertical = true;
    } else if (width() > height() * 1.15) {
        m_vertical = false;
    }
    const QSize minimum = m_vertical ? kVerticalMinimumSize : kHorizontalMinimumSize;
    if (minimumSize() != minimum) {
        setMinimumSize(minimum);
    }
}

QPainterPath DesktopLyricsWindow::lyricPath(const QString &text, const QFont &font, bool vertical)
{
    QPainterPath result;
    if (!vertical) {
        result.addText(0, 0, font, text.simplified());
    } else {
        const QString content = text.simplified();
        const QFontMetricsF metrics(font);
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, content);
        qreal offset = 0.0;
        int index = 0;
        while (index < content.size()) {
            if (content.at(index).isSpace()) {
                offset += metrics.height() * 0.45;
                ++index;
                continue;
            }
            const bool latin = isLatinCharacter(content.at(index));
            int end = index + 1;
            if (latin) {
                while (end < content.size() && isLatinCharacter(content.at(end))) {
                    ++end;
                }
            } else {
                // Preserve complete grapheme clusters, including surrogate pairs and combining marks.
                finder.setPosition(index);
                const int boundary = finder.toNextBoundary();
                if (boundary > index) {
                    end = boundary;
                }
            }
            QPainterPath token;
            token.addText(0, 0, font, content.mid(index, end - index));
            if (latin) {
                QTransform rotation;
                rotation.rotate(90);
                token = rotation.map(token);
            }
            const QRectF bounds = token.boundingRect();
            token.translate(-bounds.center().x(), offset - bounds.top());
            result.addPath(token);
            offset += latin ? qMax(metrics.height(), bounds.height() + metrics.height() * 0.12)
                            : metrics.height() * 1.02;
            index = end;
        }
    }
    const QRectF bounds = result.boundingRect();
    result.translate(-bounds.left(), -bounds.top());
    return result;
}

void DesktopLyricsWindow::rebuildPaths()
{
    const QRectF area = lyricsArea();
    QFont font = m_lyricsFont;
    font.setWeight(QFont::Normal);
    const qreal fittedSize = m_vertical ? qMin(area.width() / 3.2, area.height() / 16.0)
                                        : qMin(area.height() / 2.6, area.width() / 20.0);
    const int pixels = qBound(12, qRound(fittedSize), 96);
    font.setPixelSize(pixels);
    const QFontMetricsF metrics(font);
    m_characterWidth = metrics.horizontalAdvance(QStringLiteral("汉"));
    // Match the character advance used to construct vertical Chinese lyrics.
    m_characterHeight = metrics.height() * 1.02;
    m_lineHeight = metrics.height();
    m_currentPath = lyricPath(m_current.text, font, m_vertical);
    // Playback only changes fill, keeping both lines at the same font size and weight.
    m_nextPath = lyricPath(m_next, font, m_vertical);
    m_imagesDirty = true;
    // Long lines scroll instead of changing font size each time the song reaches a longer sentence.
    update();
}

void DesktopLyricsWindow::rebuildImages(const QColor &base, const QColor &played,
                                       qreal devicePixelRatio)
{
    m_baseImages = {};
    m_playedImage = {};
    const QColor shadowColor = base.lightnessF() > 0.5 ? QColor(0, 0, 0, 95)
                                                     : QColor(255, 255, 255, 95);
    for (int slot = 0; slot < 2; ++slot) {
        const QPainterPath &path = slot == 0 ? m_currentPath : m_nextPath;
        if (path.isEmpty()) {
            continue;
        }
        const QRectF bounds = path.boundingRect();
        // Include antialiasing and the one-pixel shadow without moving the cached path.
        const QSize pixelSize(qCeil((bounds.width() + 4.0) * devicePixelRatio),
                              qCeil((bounds.height() + 4.0) * devicePixelRatio));
        // Extremely long lyrics use direct drawing rather than allocating unbounded images.
        constexpr qint64 kMaxImagePixels = 4 * 1024 * 1024;
        if (qint64(pixelSize.width()) * pixelSize.height() > kMaxImagePixels) {
            continue;
        }
        QPixmap image(pixelSize);
        if (image.isNull()) {
            continue;
        }
        image.setDevicePixelRatio(devicePixelRatio);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.translate(2.0, 2.0);
            painter.translate(1.0, 1.0);
            painter.fillPath(path, shadowColor);
            painter.translate(-1.0, -1.0);
            painter.fillPath(path, base);
        }
        m_baseImages.at(slot) = image;
        if (slot == 0) {
            QPixmap highlight(pixelSize);
            if (!highlight.isNull()) {
                highlight.setDevicePixelRatio(devicePixelRatio);
                highlight.fill(Qt::transparent);
                QPainter painter(&highlight);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.translate(2.0, 2.0);
                painter.fillPath(path, played);
                painter.end();
                m_playedImage = highlight;
            }
        }
    }
    m_cachedBase = base;
    m_cachedPlayed = played;
    m_cachedDevicePixelRatio = devicePixelRatio;
    m_imagesDirty = false;
}

void DesktopLyricsWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // A zero-alpha layered window cannot receive mouse input in its empty areas on Windows.
    painter.fillRect(rect(), QColor(0, 0, 0, 1));
    if (m_hovered) {
        painter.setPen(QPen(QColor(180, 171, 220, 100), 1));
        painter.setBrush(QColor(20, 23, 35, 175));
        painter.drawRect(rect().adjusted(1, 1, -1, -1));
    }
    const QColor base = m_colorIndex >= 0 ? colorPresets().at(m_colorIndex).base : m_defaultBase;
    const QColor played = m_colorIndex >= 0 ? colorPresets().at(m_colorIndex).played : m_defaultPlayed;
    const qreal pixelRatio = devicePixelRatioF();
    if (m_imagesDirty || m_cachedBase != base || m_cachedPlayed != played ||
        !qFuzzyCompare(m_cachedDevicePixelRatio, pixelRatio)) {
        rebuildImages(base, played, pixelRatio);
    }
    for (int slot = 0; slot < 2; ++slot) {
        const QPainterPath &path = slot == 0 ? m_currentPath : m_nextPath;
        if (path.isEmpty()) {
            continue;
        }
        const QRectF bounds = path.boundingRect();
        const int displaySlot = slot == 0 ? m_currentSlot : 1 - m_currentSlot;
        const QRectF area = lineArea(displaySlot);
        const qreal available = m_vertical ? area.height() : area.width();
        const qreal length = m_vertical ? bounds.height() : bounds.width();
        const qreal scroll = qMax(0.0, length - available) * (slot == 0 ? m_scrollProgress : 0.0);
        painter.save();
        if (m_vertical) {
            painter.setClipRect(area);
            painter.translate(area.center().x() - bounds.width() / 2.0,
                              area.top() - scroll);
        } else {
            painter.setClipRect(area);
            painter.translate(area.left() - scroll, area.center().y() - bounds.height() / 2.0);
        }
        const QPixmap &baseImage = m_baseImages.at(slot);
        if (!baseImage.isNull()) {
            painter.drawPixmap(QPointF(-2.0, -2.0), baseImage);
        } else {
            const QColor shadowColor = base.lightnessF() > 0.5 ? QColor(0, 0, 0, 95)
                                                             : QColor(255, 255, 255, 95);
            painter.save();
            painter.translate(1.0, 1.0);
            painter.fillPath(path, shadowColor);
            painter.restore();
            painter.fillPath(path, base);
        }
        if (slot == 0 && m_progress > 0.0) {
            const QRectF fill = path.boundingRect();
            const QRectF highlight = m_vertical
                                         ? QRectF(fill.left() - 1, fill.top(),
                                                  fill.width() + 2, fill.height() * m_progress)
                                         : QRectF(fill.left(), fill.top() - 1,
                                                  fill.width() * m_progress, fill.height() + 2);
            painter.setClipRect(highlight, Qt::IntersectClip);
            if (!m_playedImage.isNull()) {
                painter.drawPixmap(QPointF(-2.0, -2.0), m_playedImage);
            } else {
                painter.fillPath(path, played);
            }
        }
        painter.restore();
    }
}

void DesktopLyricsWindow::enterEvent(QEnterEvent *event)
{
    m_hovered = true;
    m_toolbar->setVisible(m_hovered);
    update();
    QWidget::enterEvent(event);
}

void DesktopLyricsWindow::leaveEvent(QEvent *event)
{
    if (!m_dragging) {
        m_hovered = false;
        m_toolbar->hide();
        unsetCursor();
        update();
    }
    QWidget::leaveEvent(event);
}

Qt::Edges DesktopLyricsWindow::edgesAt(const QPoint &position) const
{
    Qt::Edges edges;
    if (position.x() < kEdgeWidth) {
        edges |= Qt::LeftEdge;
    } else if (position.x() >= width() - kEdgeWidth) {
        edges |= Qt::RightEdge;
    }
    if (position.y() < kEdgeWidth) {
        edges |= Qt::TopEdge;
    } else if (position.y() >= height() - kEdgeWidth) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

void DesktopLyricsWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_resizeEdges = edgesAt(event->position().toPoint());
        m_dragOrigin = event->globalPosition().toPoint();
        m_initialGeometry = geometry();
        grabMouse();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DesktopLyricsWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const QPoint delta = event->globalPosition().toPoint() - m_dragOrigin;
        QRect bounds = m_initialGeometry;
        if (m_resizeEdges == Qt::Edges()) {
            bounds.translate(delta);
        } else {
            if (m_resizeEdges.testFlag(Qt::LeftEdge)) {
                bounds.setLeft(qMin(bounds.right() - minimumWidth() + 1,
                                    m_initialGeometry.left() + delta.x()));
            }
            if (m_resizeEdges.testFlag(Qt::RightEdge)) {
                bounds.setRight(qMax(bounds.left() + minimumWidth() - 1,
                                     m_initialGeometry.right() + delta.x()));
            }
            if (m_resizeEdges.testFlag(Qt::TopEdge)) {
                bounds.setTop(qMin(bounds.bottom() - minimumHeight() + 1,
                                   m_initialGeometry.top() + delta.y()));
            }
            if (m_resizeEdges.testFlag(Qt::BottomEdge)) {
                bounds.setBottom(qMax(bounds.top() + minimumHeight() - 1,
                                      m_initialGeometry.bottom() + delta.y()));
            }
        }
        setGeometry(bounds);
        event->accept();
        return;
    }
    const Qt::Edges edges = edgesAt(event->position().toPoint());
    if ((edges.testFlag(Qt::TopEdge) && edges.testFlag(Qt::LeftEdge)) ||
        (edges.testFlag(Qt::BottomEdge) && edges.testFlag(Qt::RightEdge))) {
        setCursor(Qt::SizeFDiagCursor);
    } else if ((edges.testFlag(Qt::TopEdge) && edges.testFlag(Qt::RightEdge)) ||
               (edges.testFlag(Qt::BottomEdge) && edges.testFlag(Qt::LeftEdge))) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge)) {
        setCursor(Qt::SizeHorCursor);
    } else if (edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge)) {
        setCursor(Qt::SizeVerCursor);
    } else {
        setCursor(Qt::SizeAllCursor);
    }
    QWidget::mouseMoveEvent(event);
}

void DesktopLyricsWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        releaseMouse();
        keepOnScreen();
        scheduleSave();
        flushConfiguration();
        if (!rect().contains(mapFromGlobal(event->globalPosition().toPoint()))) {
            m_hovered = false;
            m_toolbar->hide();
            update();
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void DesktopLyricsWindow::resizeEvent(QResizeEvent *event)
{
    updateOrientation();
    layoutToolbar();
    rebuildPaths();
    scheduleSave();
    QWidget::resizeEvent(event);
}

void DesktopLyricsWindow::moveEvent(QMoveEvent *event)
{
    scheduleSave();
    QWidget::moveEvent(event);
}

void DesktopLyricsWindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
    hide();
    m_hovered = false;
    m_toolbar->hide();
    flushConfiguration();
    emit visibilityChanged(false);
}
