#include "player_widgets.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QIcon>
#include <QMediaMetaData>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolTip>
#include <QVariant>
#include <QWindow>

namespace PlayerWidgets {

QString styleSheetWithFontSizes(const QString &baseStyleSheet, int applicationFontSize,
                                int lyricsFontSize)
{
    QString scaledStyleSheet = baseStyleSheet;
    const QRegularExpression fontSizeExpression(QStringLiteral("font-size:\\s*(\\d+)px;"));
    const auto matches = fontSizeExpression.globalMatch(baseStyleSheet);
    QList<QRegularExpressionMatch> fontSizeMatches;
    auto iterator = matches;
    while (iterator.hasNext()) {
        fontSizeMatches.append(iterator.next());
    }

    for (auto match = fontSizeMatches.crbegin(); match != fontSizeMatches.crend(); ++match) {
        const int baseFontSize = match->captured(1).toInt();
        const int scaledFontSize = qMax(1, qRound(baseFontSize * applicationFontSize / 14.0));
        scaledStyleSheet.replace(match->capturedStart(1), match->capturedLength(1),
                                 QString::number(scaledFontSize));
    }

    scaledStyleSheet.append(
        QStringLiteral("\n#lyricsList { font-size: %1px; }\n").arg(lyricsFontSize));
    return scaledStyleSheet;
}

QString artistFromMetadata(const QMediaMetaData &metadata)
{
    const QList<QMediaMetaData::Key> artistKeys = {QMediaMetaData::ContributingArtist,
                                                   QMediaMetaData::AlbumArtist,
                                                   QMediaMetaData::LeadPerformer};
    for (const QMediaMetaData::Key key : artistKeys) {
        const QString artist = metadata.stringValue(key).trimmed();
        if (!artist.isEmpty()) {
            return artist;
        }
    }
    return {};
}

void showIconButtonToolTip(QPushButton *button, bool refresh)
{
    if (button == nullptr || !button->underMouse() || button->toolTip().isEmpty()) {
        return;
    }
    if (!refresh && button->property("iconToolTipVisible").toBool()) {
        return;
    }
    const QPoint position = QCursor::pos() + QPoint(14, 20);
    QToolTip::showText(position, button->toolTip(), button);
    button->setProperty("iconToolTipVisible", true);
}

void configureIconButton(QPushButton *button, const QString &iconPath, const QString &toolTip,
                         const QSize &iconSize)
{
    button->setText({});
    button->setProperty("iconPath", iconPath);
    button->setProperty("iconWidth", iconSize.width());
    button->setProperty("iconHeight", iconSize.height());
    button->setIcon(QIcon(iconPath));
    button->setIconSize(iconSize);
    button->setToolTip(toolTip);
    button->setAccessibleName(toolTip);
    if (button->property("immediateToolTipInstalled").toBool() == false) {
        button->installEventFilter(new ImmediateToolTipFilter(button));
        button->setProperty("immediateToolTipInstalled", true);
    }
}

QIcon tintedIcon(const QString &path, const QSize &size, const QColor &color,
                 qreal devicePixelRatio)
{
    const QPixmap source = QIcon(path).pixmap(size, devicePixelRatio);
    QPixmap tinted(source.size());
    tinted.setDevicePixelRatio(source.devicePixelRatio());
    tinted.fill(Qt::transparent);
    QPainter painter(&tinted);
    painter.drawPixmap(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), color);
    return QIcon(tinted);
}

PlaybackProgressSlider::PlaybackProgressSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent)
{
}

void PlaybackProgressSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QSlider::mousePressEvent(event);
        return;
    }
    if (maximum() <= minimum()) {
        event->ignore();
        return;
    }
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const QPoint position = event->position().toPoint();
    // 拖动滑块时保留按下位置；点击轨道时让滑块中心对准点击位置。
    m_dragOffset = handle.contains(position) ? position.x() - handle.left() : handle.width() / 2;
    setFocus(Qt::MouseFocusReason);
    setSliderDown(true);
    setPositionFromPoint(position);
    event->accept();
}

void PlaybackProgressSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (isSliderDown() && event->buttons().testFlag(Qt::LeftButton)) {
        setPositionFromPoint(event->position().toPoint());
        event->accept();
        return;
    }
    QSlider::mouseMoveEvent(event);
}

void PlaybackProgressSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && isSliderDown()) {
        setPositionFromPoint(event->position().toPoint());
        setSliderDown(false);
        event->accept();
        return;
    }
    QSlider::mouseReleaseEvent(event);
}

void PlaybackProgressSlider::setPositionFromPoint(const QPoint &position)
{
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect groove =
        style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const int span = qMax(0, groove.width() - handle.width());
    const int offset = qBound(0, position.x() - m_dragOffset - groove.left(), span);
    // 复用 sliderMoved 信号，点击与拖动都走相同的跳转和音效缓冲清理逻辑。
    setSliderPosition(
        QStyle::sliderValueFromPosition(minimum(), maximum(), offset, span, option.upsideDown));
}

PopupToggleEventFilter::PopupToggleEventFilter(QPushButton *button, QMenu *menu)
    : QObject(button), m_button(button), m_menu(menu)
{
}

bool PopupToggleEventFilter::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress && m_menu->isVisible()) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const QRect buttonRect(m_button->mapToGlobal(QPoint(0, 0)), m_button->size());
        if (buttonRect.contains(mouseEvent->globalPosition().toPoint())) {
            m_menu->hide();
            return true;
        }
    }

    return QObject::eventFilter(watched, event);
}

ImmediateToolTipFilter::ImmediateToolTipFilter(QPushButton *button)
    : QObject(button), m_button(button)
{
}

bool ImmediateToolTipFilter::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_button && event->type() == QEvent::Enter) {
        m_button->setProperty("iconToolTipVisible", false);
        showIconButtonToolTip(m_button);
    }
    if (watched == m_button && event->type() == QEvent::Leave) {
        QToolTip::hideText();
        m_button->setProperty("iconToolTipVisible", false);
    }
    if (watched == m_button && event->type() == QEvent::ToolTip) {
        return true;
    }
    return QObject::eventFilter(watched, event);
}

FramelessWindowEventFilter::FramelessWindowEventFilter(QWidget *window, QWidget *titleBar)
    : QObject(window), m_window(window), m_titleBar(titleBar)
{
}

bool FramelessWindowEventFilter::eventFilter(QObject *watched, QEvent *event)
{
    auto *widget = qobject_cast<QWidget *>(watched);
    if (widget == nullptr || widget->window() != m_window || !m_window->isVisible()) {
        return QObject::eventFilter(watched, event);
    }

    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick) {
        return QObject::eventFilter(watched, event);
    }
    auto *mouseEvent = static_cast<QMouseEvent *>(event);
    if (mouseEvent->button() != Qt::LeftButton) {
        return QObject::eventFilter(watched, event);
    }

    const QPoint globalPosition = mouseEvent->globalPosition().toPoint();
    const QPoint localPosition = m_window->mapFromGlobal(globalPosition);
    if (!m_window->isMaximized() && event->type() == QEvent::MouseButtonPress) {
        constexpr int resizeBorder = 7;
        Qt::Edges edges;
        if (localPosition.x() < resizeBorder) {
            edges |= Qt::LeftEdge;
        } else if (localPosition.x() >= m_window->width() - resizeBorder) {
            edges |= Qt::RightEdge;
        }
        if (localPosition.y() < resizeBorder) {
            edges |= Qt::TopEdge;
        } else if (localPosition.y() >= m_window->height() - resizeBorder) {
            edges |= Qt::BottomEdge;
        }
        if (edges != Qt::Edges() && m_window->windowHandle() != nullptr) {
            return m_window->windowHandle()->startSystemResize(edges);
        }
    }

    const QRect titleBarRect(m_titleBar->mapToGlobal(QPoint(0, 0)), m_titleBar->size());
    if (!titleBarRect.contains(globalPosition)) {
        return QObject::eventFilter(watched, event);
    }
    QWidget *target = QApplication::widgetAt(globalPosition);
    while (target != nullptr && target != m_titleBar && target != m_window) {
        if (qobject_cast<QPushButton *>(target) != nullptr) {
            return QObject::eventFilter(watched, event);
        }
        target = target->parentWidget();
    }
    if (event->type() == QEvent::MouseButtonDblClick) {
        m_window->isMaximized() ? m_window->showNormal() : m_window->showMaximized();
        return true;
    }
    if (m_window->windowHandle() != nullptr) {
        return m_window->windowHandle()->startSystemMove();
    }
    return QObject::eventFilter(watched, event);
}

CaptionIconHoverFilter::CaptionIconHoverFilter(QPushButton *button)
    : QObject(button), m_button(button)
{
}

bool CaptionIconHoverFilter::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_button) {
        return QObject::eventFilter(watched, event);
    }
    const QString path = m_button->property("iconPath").toString();
    const QSize size(m_button->property("iconWidth").toInt(),
                     m_button->property("iconHeight").toInt());
    if (event->type() == QEvent::Enter) {
        m_button->setIcon(tintedIcon(path, size, Qt::white, m_button->devicePixelRatioF()));
    } else if (event->type() == QEvent::Leave) {
        const QColor color = m_button->property("normalIconColor").value<QColor>();
        m_button->setIcon(tintedIcon(path, size, color, m_button->devicePixelRatioF()));
    }
    return QObject::eventFilter(watched, event);
}

} // namespace PlayerWidgets
