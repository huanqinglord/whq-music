#include "player_widgets.h"

#include <QDateTime>
#include <QFileInfo>
#include <QDebug>
#include <QFutureWatcher>
#include <QtConcurrentRun>
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

namespace {
QString cachedSamplePath;
QDateTime cachedSampleModified;
qint64 cachedSampleBytes = -1;
QImage cachedSampleImage;

// 三次可分离均值模糊近似柔和的高斯效果；仅操作工作线程自己的 QImage。
QImage blurBackground(QImage image, int radius)
{
    image = image.convertToFormat(QImage::Format_RGB32);
    for (int pass = 0; pass < 3; ++pass) {
        for (int axis = 0; axis < 2; ++axis) {
            QImage output(image.size(), QImage::Format_RGB32);
            if (output.isNull()) {
                return {};
            }
            const int length = axis == 0 ? image.width() : image.height();
            const int lines = axis == 0 ? image.height() : image.width();
            for (int line = 0; line < lines; ++line) {
                const auto pixel = [&](int position) {
                    const int bounded = qBound(0, position, length - 1);
                    return axis == 0 ? image.pixel(bounded, line) : image.pixel(line, bounded);
                };
                int red = 0;
                int green = 0;
                int blue = 0;
                const auto accumulate = [&](QRgb color, int sign) {
                    red += sign * qRed(color);
                    green += sign * qGreen(color);
                    blue += sign * qBlue(color);
                };
                for (int offset = -radius; offset <= radius; ++offset) {
                    accumulate(pixel(offset), 1);
                }
                const int count = radius * 2 + 1;
                for (int position = 0; position < length; ++position) {
                    const QRgb color = qRgb(red / count, green / count, blue / count);
                    output.setPixel(axis == 0 ? position : line,
                                    axis == 0 ? line : position, color);
                    accumulate(pixel(position - radius), -1);
                    accumulate(pixel(position + radius + 1), 1);
                }
            }
            image = output;
        }
    }
    return image;
}
}

void cacheBackgroundSample(const QString &path, const QImage &image)
{
    // 仅在主线程发布和读取缓存，后台解码不持有界面对象。
    const QFileInfo file(path);
    cachedSamplePath = path;
    cachedSampleModified = file.lastModified();
    cachedSampleBytes = file.size();
    cachedSampleImage = image;
}

QImage backgroundSample(const QString &path)
{
    const QFileInfo file(path);
    if (path != cachedSamplePath || file.lastModified() != cachedSampleModified ||
        file.size() != cachedSampleBytes || cachedSampleImage.isNull()) {
        cacheBackgroundSample(path, loadScaledImage(path, QSize(384, 384)));
    }
    return cachedSampleImage;
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
    QImageReader reader(path);
    if (!reader.canRead()) {
        return false;
    }
    m_backgroundPath = path;
    ++m_backgroundGeneration;
    updateBackgroundTone();
    rebuildBlurredBackground();
    update();
    return true;
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
    if (m_sharpBackground.isNull()) {
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
        return;
    }
    const quint64 generation = ++m_backgroundGeneration;
    const QString path = m_backgroundPath;
    const QSize logicalSize = size();
    const qreal scale = devicePixelRatioF();
    const QColor edgeColor = m_edgeColor;
    using BackgroundImages = QPair<QImage, QImage>;
    auto *watcher = new QFutureWatcher<BackgroundImages>(this);
    connect(watcher, &QFutureWatcher<BackgroundImages>::finished, this,
            [this, watcher, generation] {
                const auto images = watcher->result();
                watcher->deleteLater();
                // 过期的尺寸或背景任务不得覆盖后一次设置；销毁窗口会断开回调。
                if (generation != m_backgroundGeneration) {
                    return;
                }
                if (images.first.isNull() || images.second.isNull()) {
                    qWarning() << "Failed to prepare window background";
                    return;
                }
                m_sharpBackground = QPixmap::fromImage(images.first);
                m_blurredBackground = QPixmap::fromImage(images.second);
                update();
            });
    watcher->setFuture(QtConcurrent::run([path, logicalSize, scale, edgeColor] {
        QImageReader reader(path);
        const QSize sourceSize = reader.size();
        const QSize physicalSize = logicalSize * scale;
        const QSize required = sourceSize.isValid()
                                   ? sourceSize.scaled(physicalSize, Qt::KeepAspectRatioByExpanding)
                                   : physicalSize;
        const QImage source = loadScaledImage(path, required);
        if (source.isNull()) {
            return BackgroundImages{};
        }
        QImage canvas(physicalSize, QImage::Format_ARGB32_Premultiplied);
        if (canvas.isNull()) {
            return BackgroundImages{};
        }
        canvas.fill(edgeColor);
        {
            QPainter painter(&canvas);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            const QSizeF scaled = QSizeF(source.size()).scaled(physicalSize, Qt::KeepAspectRatioByExpanding);
            painter.drawImage(QRectF((physicalSize.width() - scaled.width()) / 2,
                                     (physicalSize.height() - scaled.height()) / 2,
                                     scaled.width(), scaled.height()), source);
        }
        const QSize blurSize(qMax(1, physicalSize.width() / 2),
                             qMax(1, physicalSize.height() / 2));
        QImage blurred = blurBackground(
            canvas.scaled(blurSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
            qMax(1, qRound(7 * scale)));
        canvas.setDevicePixelRatio(scale);
        return BackgroundImages{canvas, blurred};
    }));
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
