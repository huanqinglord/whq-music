#include "player_widgets.h"

#include <QEvent>
#include <QHideEvent>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QtMath>

namespace PlayerWidgets {

TraySongLabel::TraySongLabel(QWidget *parent) : QLabel(parent), m_scrollTimer(this)
{
    m_scrollTimer.setInterval(16);
    connect(&m_scrollTimer, &QTimer::timeout, this, [this] {
        update();
    });
}

void TraySongLabel::setSongName(const QString &songName)
{
    if (text() == songName) {
        return;
    }
    setText(songName);
    restartScroll();
}

void TraySongLabel::showEvent(QShowEvent *event)
{
    QLabel::showEvent(event);
    restartScroll();
}

void TraySongLabel::hideEvent(QHideEvent *event)
{
    m_scrollTimer.stop();
    QLabel::hideEvent(event);
}

void TraySongLabel::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    restartScroll();
}

void TraySongLabel::changeEvent(QEvent *event)
{
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        restartScroll();
    }
}

void TraySongLabel::paintEvent(QPaintEvent *event)
{
    const QRect area = contentsRect();
    const int overflow = m_textWidth - area.width();
    if (overflow <= 0) {
        QLabel::paintEvent(event);
        return;
    }

    // 按实际经过时间计算位移，避免界面繁忙时滚动速度随定时器波动。
    const qint64 travelMs = qMax<qint64>(1, overflow * 1000LL / ScrollPixelsPerSecond);
    const qint64 cycleMs = 2 * (EndpointPauseMs + travelMs);
    const qint64 phaseMs = m_scrollElapsed.isValid() ? m_scrollElapsed.elapsed() % cycleMs : 0;
    qreal offset = 0;
    if (phaseMs >= EndpointPauseMs + travelMs + EndpointPauseMs) {
        const qint64 returnMs = phaseMs - EndpointPauseMs - travelMs - EndpointPauseMs;
        offset = overflow * (1.0 - static_cast<qreal>(returnMs) / travelMs);
    } else if (phaseMs >= EndpointPauseMs + travelMs) {
        offset = overflow;
    } else if (phaseMs >= EndpointPauseMs) {
        offset = overflow * static_cast<qreal>(phaseMs - EndpointPauseMs) / travelMs;
    }

    QPainter painter(this);
    painter.setClipRect(area);
    painter.setFont(font());
    painter.setPen(palette().color(QPalette::WindowText));
    painter.drawText(QRectF(area.left() - offset, area.top(), m_textWidth, area.height()),
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text());
}

void TraySongLabel::restartScroll()
{
    m_scrollTimer.stop();
    m_textWidth = fontMetrics().size(Qt::TextSingleLine, text()).width();
    m_scrollElapsed.start();
    if (isVisible() && m_textWidth > contentsRect().width()) {
        m_scrollTimer.start();
    }
    update();
}

TrayPlaybackPanel::TrayPlaybackPanel(QWidget *parent)
    : QWidget(parent)
{
    const qreal scale = devicePixelRatioF();
    const QSize bounds(qCeil(156 * scale), qCeil(185 * scale));
    m_background = QPixmap::fromImage(
        loadScaledImage(QStringLiteral(":/backgrounds/tray_icon_background.png"), bounds));
}

void TrayPlaybackPanel::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(255, 247, 234));
    if (!m_background.isNull()) {
        // 完整保留标志比例，窄高面板上下延续浅色背景。
        const QPixmap background =
            m_background.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        painter.save();
        painter.setOpacity(0.25);
        painter.drawPixmap((width() - background.width()) / 2, (height() - background.height()) / 2,
                           background);
        painter.restore();
    }
    painter.fillRect(rect(), QColor(255, 250, 242, 64));
}

} // namespace PlayerWidgets
