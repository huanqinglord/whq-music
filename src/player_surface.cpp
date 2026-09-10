#include "player_widgets.h"

#include <QDateTime>
#include <QFileInfo>
#include <QGraphicsBlurEffect>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QImage>
#include <QImageReader>
#include <QMediaPlayer>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QtMath>

namespace PlayerWidgets {

QImage loadScaledImage(const QString &path, const QSize &bounds)
{
    if (path.isEmpty() || bounds.isEmpty()) {
        return {};
    }
    QImageReader reader(path);
    const QSize originalSize = reader.size();
    if (originalSize.isValid() &&
        (originalSize.width() > bounds.width() || originalSize.height() > bounds.height())) {
        reader.setScaledSize(originalSize.scaled(bounds, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    // Some decoders cannot scale while decoding; retain only the reduced result.
    if (!image.isNull() &&
        (image.width() > bounds.width() || image.height() > bounds.height())) {
        image = image.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

QImage backgroundSample(const QString &path)
{
    // One shared small image serves all UI color sampling; never cache full source images here.
    static QString cachedPath;
    static QDateTime cachedModified;
    static qint64 cachedBytes = -1;
    static QImage cachedImage;
    const QFileInfo file(path);
    const QDateTime modified = file.lastModified();
    const qint64 bytes = file.size();
    if (path != cachedPath || modified != cachedModified || bytes != cachedBytes ||
        cachedImage.isNull()) {
        cachedImage = loadScaledImage(path, QSize(384, 384));
        cachedPath = path;
        cachedModified = modified;
        cachedBytes = bytes;
    }
    return cachedImage;
}

QueueDrawer::QueueDrawer(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, false);
}

void QueueDrawer::setBackdropProvider(const std::function<QPixmap(const QWidget *)> &provider)
{
    m_backdropProvider = provider;
    update();
}

void QueueDrawer::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    // 动画期间按当前位置取图，避免固定截图随抽屉移动或被拉伸。
    const QPixmap backdrop = m_backdropProvider ? m_backdropProvider(this) : QPixmap();
    if (!backdrop.isNull()) {
        painter.drawPixmap(QPoint(0, 0), backdrop);
    } else {
        painter.fillRect(rect(), QColor(18, 21, 30, 225));
    }
    painter.fillRect(rect(), QColor(10, 12, 18, 105));
    painter.fillRect(QRect(0, 0, 1, height()), QColor(255, 255, 255, 42));
}

AppSurface::AppSurface(QWidget *parent) : QWidget(parent), m_backgroundResizeTimer(this)
{
    m_backgroundResizeTimer.setSingleShot(true);
    m_backgroundResizeTimer.setInterval(120);
    connect(&m_backgroundResizeTimer, &QTimer::timeout, this, [this] {
        rebuildBlurredBackground();
        update();
    });
}

bool AppSurface::setBackground(const QString &path)
{
    m_backgroundResizeTimer.stop();
    m_backgroundPath = path;
    QImageReader reader(path);
    m_backgroundSourceSize = reader.size();
    m_background = {};
    updateBackgroundTone();
    rebuildBlurredBackground();
    update();
    return !m_sharpBackground.isNull();
}

void AppSurface::setBlurTargets(QWidget *content, QWidget *playerBar)
{
    m_content = content;
    m_playerBar = playerBar;
}

void AppSurface::setContentBlurred(bool blurred)
{
    m_contentBlurred = blurred;
    update();
}

QPixmap AppSurface::blurredRegion(const QWidget *target) const
{
    if (target == nullptr || m_blurredBackground.isNull()) {
        return {};
    }
    const QRect area(target->mapTo(this, QPoint(0, 0)), target->size());
    const QRect visibleArea = area.intersected(rect());
    // copy 的空矩形表示复制整张图片，窗口外的控件必须提前返回。
    if (visibleArea.isEmpty()) {
        return {};
    }
    // 保留控件尺寸和裁剪偏移，确保抽屉部分进入窗口时背景仍然对齐。
    QPixmap backdrop(target->size() * devicePixelRatioF());
    backdrop.setDevicePixelRatio(devicePixelRatioF());
    backdrop.fill(Qt::transparent);
    QPainter painter(&backdrop);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(QRectF(visibleArea.translated(-area.topLeft())), m_blurredBackground,
                       blurredSourceRect(visibleArea));
    return backdrop;
}

QRectF AppSurface::blurredSourceRect(const QRect &area) const
{
    const qreal xScale = static_cast<qreal>(m_blurredBackground.width()) / qMax(1, width());
    const qreal yScale = static_cast<qreal>(m_blurredBackground.height()) / qMax(1, height());
    return QRectF(area.x() * xScale, area.y() * yScale, area.width() * xScale,
                  area.height() * yScale);
}

void AppSurface::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), QColor(QStringLiteral("#0b0d14")));
    if (m_background.isNull()) {
        return;
    }
    painter.drawPixmap(rect(), m_sharpBackground);

    const auto paintBlurredArea = [this, &painter](QWidget *target) {
        if (target == nullptr) {
            return;
        }
        const QRect area(target->mapTo(this, QPoint(0, 0)), target->size());
        painter.drawPixmap(QRectF(area), m_blurredBackground, blurredSourceRect(area));
        painter.fillRect(area, m_darkBackground ? QColor(8, 10, 16, 72) : QColor(8, 10, 16, 118));
        painter.fillRect(area, QColor(255, 255, 255, 10));
    };
    if (m_contentBlurred) {
        paintBlurredArea(m_content);
    }
    paintBlurredArea(m_playerBar);
}

void AppSurface::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_sharpBackground.isNull()) {
        rebuildBlurredBackground();
    } else {
        // During dragging, scale the last completed frame instead of rebuilding the blur.
        m_backgroundResizeTimer.start();
    }
    update();
}

void AppSurface::rebuildBlurredBackground()
{
    if (m_backgroundPath.isEmpty() || size().isEmpty()) {
        m_sharpBackground = {};
        m_blurredBackground = {};
        return;
    }

    // Round physical pixel dimensions into buckets to avoid decoding on every resize pixel.
    const qreal scale = devicePixelRatioF();
    const QSize bounds(((qCeil(width() * scale) + 255) / 256) * 256,
                       ((qCeil(height() * scale) + 255) / 256) * 256);
    QSize required = m_backgroundSourceSize.isValid()
                         ? m_backgroundSourceSize.scaled(bounds, Qt::KeepAspectRatioByExpanding)
                         : bounds;
    if (m_backgroundSourceSize.isValid() &&
        required.width() > m_backgroundSourceSize.width()) {
        required = m_backgroundSourceSize;
    }
    if (m_background.isNull() || m_background.width() < required.width() ||
        m_background.height() < required.height() ||
        m_background.width() > required.width() * 1.5 ||
        m_background.height() > required.height() * 1.5) {
        m_background = QPixmap::fromImage(loadScaledImage(m_backgroundPath, required));
    }
    if (m_background.isNull()) {
        m_sharpBackground = {};
        m_blurredBackground = {};
        return;
    }

    QPixmap canvas(size() * scale);
    canvas.setDevicePixelRatio(scale);
    canvas.fill(m_edgeColor);
    {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSizeF scaled = QSizeF(m_background.size()).scaled(size(), Qt::KeepAspectRatioByExpanding);
        const QRectF target((width() - scaled.width()) / 2, (height() - scaled.height()) / 2,
                            scaled.width(), scaled.height());
        painter.drawPixmap(target, m_background, QRectF(m_background.rect()));
    }
    m_sharpBackground = canvas;

    // Blur needs fewer pixels than the sharp layer; adjust the radius to retain its visual size.
    const qreal blurScale = scale * 0.5;
    const QSize blurSize(qMax(1, qCeil(width() * blurScale)),
                         qMax(1, qCeil(height() * blurScale)));
    QPixmap blurInput = canvas.scaled(blurSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    blurInput.setDevicePixelRatio(1.0);
    QGraphicsScene scene;
    scene.setSceneRect(QRectF(QPointF(0, 0), QSizeF(blurSize)));
    auto *item = scene.addPixmap(blurInput);
    auto *blur = new QGraphicsBlurEffect;
    blur->setBlurHints(QGraphicsBlurEffect::QualityHint);
    blur->setBlurRadius(28.0 * blurScale);
    item->setGraphicsEffect(blur);

    QImage blurred(blurSize, QImage::Format_ARGB32_Premultiplied);
    blurred.fill(Qt::transparent);
    QPainter painter(&blurred);
    scene.render(&painter, QRectF(blurred.rect()), scene.sceneRect());
    m_blurredBackground = QPixmap::fromImage(blurred);
}

void AppSurface::updateBackgroundTone()
{
    const QImage background = backgroundSample(m_backgroundPath);
    if (background.isNull()) {
        m_darkBackground = true;
        return;
    }
    const QImage sample = background
                              .scaled(48, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGB32);
    qreal luminance = 0.0;
    qreal red = 0.0;
    qreal green = 0.0;
    qreal blue = 0.0;
    for (int y = 0; y < sample.height(); ++y) {
        for (int x = 0; x < sample.width(); ++x) {
            const QColor color = sample.pixelColor(x, y);
            luminance += 0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF();
            red += color.redF();
            green += color.greenF();
            blue += color.blueF();
        }
    }
    const qreal pixelCount = sample.width() * sample.height();
    m_darkBackground = luminance / pixelCount < 0.55;
    m_edgeColor =
        QColor::fromRgbF(red / pixelCount, green / pixelCount, blue / pixelCount).darker(145);
}

} // namespace PlayerWidgets
