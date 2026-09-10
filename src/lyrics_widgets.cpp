#include "player_widgets.h"

#include <QEnterEvent>
#include <QEvent>
#include <QFontMetricsF>
#include <QImage>
#include <QListWidget>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>

#include <algorithm>
#include <utility>

namespace PlayerWidgets {

LyricsItemDelegate::LyricsItemDelegate(QObject *parent) : QStyledItemDelegate(parent)
{
    setBackground({});
}

bool LyricsItemDelegate::setScrollProgress(qreal progress)
{
    const qreal bounded = qBound(0.0, progress, 1.0);
    if (qFuzzyCompare(m_scrollProgress + 1.0, bounded + 1.0)) {
        return false;
    }
    m_scrollProgress = bounded;
    return true;
}

QPainterPath LyricsItemDelegate::textOutline(const QString &text, const QFont &font) const
{
    const QString key = font.toString() + QLatin1Char('\n') + text;
    if (const QPainterPath *cached = m_textOutlines.object(key)) {
        return *cached;
    }
    QPainterPath outline;
    outline.addText(QPointF(0, 0), font, text);
    const qint64 cost = static_cast<qint64>(outline.elementCount()) *
                           sizeof(QPainterPath::Element) +
                       key.size() * sizeof(QChar) + sizeof(QPainterPath);
    // Bound the cache, including very long lyrics, instead of retaining every rendered song.
    if (cost <= m_textOutlines.maxCost()) {
        m_textOutlines.insert(key, new QPainterPath(outline), static_cast<int>(cost));
    }
    return outline;
}

bool LyricsItemDelegate::needsHorizontalScroll(const QModelIndex &index, int width) const
{
    const auto *lyrics = qobject_cast<const QListWidget *>(parent());
    if (lyrics == nullptr || !index.isValid()) {
        return false;
    }
    QFont font = lyrics->font();
    font.setBold(true);
    if (font.pixelSize() > 0) {
        font.setPixelSize(qRound(font.pixelSize() * 1.35));
    } else {
        font.setPointSizeF(font.pointSizeF() * 1.35);
    }
    return textOutline(index.data(Qt::DisplayRole).toString(), font).boundingRect().width() >
           qMax(0, width - 32);
}

void LyricsItemDelegate::setBackground(const QString &path)
{
    m_colors = {QColor(QStringLiteral("#9b91ff")), QColor(QStringLiteral("#74d7ff")),
                QColor(QStringLiteral("#ff83cf"))};
    m_darkBackground = true;
    m_background = backgroundSample(path);
    m_sampledBackground = {};
    m_sampledSurfaceSize = {};

    QImage image = m_background;
    if (image.isNull()) {
        return;
    }
    image = image.scaled(72, 72, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_RGB32);

    QList<QColor> candidates;
    qreal luminanceSum = 0.0;
    int pixelCount = 0;
    for (int y = 0; y < image.height(); y += 3) {
        for (int x = 0; x < image.width(); x += 3) {
            const QColor color = image.pixelColor(x, y);
            luminanceSum +=
                0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF();
            ++pixelCount;
            if (color.hsvSaturationF() >= 0.35) {
                candidates.append(color);
            }
        }
    }
    m_darkBackground = pixelCount == 0 || luminanceSum / pixelCount < 0.55;
    std::sort(candidates.begin(), candidates.end(), [](const QColor &left, const QColor &right) {
        return left.hsvSaturationF() * left.valueF() > right.hsvSaturationF() * right.valueF();
    });

    QList<QColor> selected;
    for (QColor color : std::as_const(candidates)) {
        bool sufficientlyDifferent = true;
        for (const QColor &existing : std::as_const(selected)) {
            if (qAbs(color.hsvHueF() - existing.hsvHueF()) < 0.12) {
                sufficientlyDifferent = false;
                break;
            }
        }
        if (!sufficientlyDifferent) {
            continue;
        }
        color.setHsvF(color.hsvHueF(), qMax(0.58, color.hsvSaturationF()),
                      m_darkBackground ? qMax(0.82, color.valueF()) : qMin(0.62, color.valueF()));
        selected.append(color);
        if (selected.size() == 3) {
            break;
        }
    }
    if (selected.size() >= 2) {
        m_colors = selected;
    }
}

void LyricsItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                               const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const bool active = index.data(Qt::UserRole + 1).toBool();
    const auto *lyrics = qobject_cast<const QListWidget *>(parent());
    QRectF textArea = option.rect;
    if (lyrics != nullptr) {
        textArea.setLeft(0);
        textArea.setWidth(lyrics->viewport()->width());
    }
    textArea.adjust(16, 0, -16, 0);
    painter->setClipRect(textArea, Qt::IntersectClip);

    QFont font = option.font;
    font.setBold(active);
    if (active) {
        if (font.pixelSize() > 0) {
            font.setPixelSize(qRound(font.pixelSize() * 1.35));
        } else {
            font.setPointSizeF(font.pointSizeF() * 1.35);
        }
    }
    painter->setFont(font);
    QString text = index.data(Qt::DisplayRole).toString();
    const QFontMetricsF metrics(font);
    if (!active) {
        text = metrics.elidedText(text, Qt::ElideRight, qMax(0.0, textArea.width()));
    }
    const QPainterPath textPath = textOutline(text, font);
    const QRectF bounds = textPath.boundingRect();
    qreal left = textArea.center().x() - bounds.width() / 2.0;
    if (active && bounds.width() > textArea.width()) {
        // 从句首移到句尾，始终裁剪在可见区域内，不扩大列表的横向范围。
        left = textArea.left() - (bounds.width() - textArea.width()) * m_scrollProgress;
    }
    const QPointF origin(left - bounds.left(),
                         option.rect.center().y() + (metrics.ascent() - metrics.descent()) / 2.0);
    // Move the painter, keeping the cached outline shared rather than copying its elements.
    painter->translate(origin);

    const qreal localLuminance = backgroundLuminance(option.rect);
    const bool locallyDark = localLuminance < 0.52;
    if (active) {
        const QColor outline = locallyDark ? QColor(5, 7, 12, 180) : QColor(255, 255, 255, 175);
        painter->strokePath(textPath,
                            QPen(outline, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }

    if (active) {
        const QList<QColor> adaptiveColors = locallyDark
                                                 ? QList<QColor>{QColor(QStringLiteral("#fff176")),
                                                                 QColor(QStringLiteral("#72e6ff")),
                                                                 QColor(QStringLiteral("#ff91d0"))}
                                                 : QList<QColor>{QColor(QStringLiteral("#5b21b6")),
                                                                 QColor(QStringLiteral("#075985")),
                                                                 QColor(QStringLiteral("#9d174d"))};
        QLinearGradient gradient(QPointF(option.rect.topLeft()) - origin,
                                 QPointF(option.rect.topRight()) - origin);
        for (int i = 0; i < adaptiveColors.size(); ++i) {
            gradient.setColorAt(static_cast<qreal>(i) / (adaptiveColors.size() - 1),
                                adaptiveColors.at(i));
        }
        painter->fillPath(textPath, gradient);
    } else {
        painter->fillPath(textPath,
                          locallyDark ? QColor(248, 249, 255, 225) : QColor(17, 19, 27, 225));
    }
    painter->restore();
}

QSize LyricsItemDelegate::sizeHint(const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    // 行宽由视口决定，长歌词只在当前行内部移动。
    size.setWidth(1);
    size.setHeight(index.data(Qt::UserRole + 1).toBool() ? 68 : 48);
    return size;
}

qreal LyricsItemDelegate::backgroundLuminance(const QRect &itemRect) const
{
    const auto *lyrics = qobject_cast<const QListWidget *>(parent());
    if (lyrics == nullptr || m_background.isNull()) {
        return m_darkBackground ? 0.2 : 0.8;
    }

    const QWidget *surface = lyrics;
    while (surface->parentWidget() != nullptr &&
           surface->objectName() != QStringLiteral("appSurface")) {
        surface = surface->parentWidget();
    }
    if (surface->objectName() != QStringLiteral("appSurface") || surface->size().isEmpty()) {
        return m_darkBackground ? 0.2 : 0.8;
    }

    if (m_sampledBackground.isNull() || m_sampledSurfaceSize != surface->size()) {
        const QSize sampleSize(qMax(1, surface->width() / 8), qMax(1, surface->height() / 8));
        QImage canvas(sampleSize, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        QPainter samplePainter(&canvas);
        const QImage scaled = m_background.scaled(sampleSize, Qt::KeepAspectRatioByExpanding,
                                                  Qt::SmoothTransformation);
        const QPoint origin((sampleSize.width() - scaled.width()) / 2,
                            (sampleSize.height() - scaled.height()) / 2);
        samplePainter.drawImage(origin, scaled);
        m_sampledBackground = canvas;
        m_sampledSurfaceSize = surface->size();
    }

    const QPoint surfacePosition =
        lyrics->viewport()->mapTo(const_cast<QWidget *>(surface), itemRect.topLeft());
    QRect sampleRect(qRound(surfacePosition.x() / 8.0), qRound(surfacePosition.y() / 8.0),
                     qMax(1, qRound(itemRect.width() / 8.0)),
                     qMax(1, qRound(itemRect.height() / 8.0)));
    sampleRect = sampleRect.intersected(m_sampledBackground.rect());
    if (sampleRect.isEmpty()) {
        return m_darkBackground ? 0.2 : 0.8;
    }

    qreal luminance = 0.0;
    int samples = 0;
    for (int y = sampleRect.top(); y <= sampleRect.bottom(); ++y) {
        for (int x = sampleRect.left(); x <= sampleRect.right(); ++x) {
            const QColor color = m_sampledBackground.pixelColor(x, y);
            luminance += 0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF();
            ++samples;
        }
    }
    return samples > 0 ? luminance / samples : (m_darkBackground ? 0.2 : 0.8);
}

LyricsScriptButton::LyricsScriptButton(QWidget *parent) : QPushButton(parent)
{
    setAttribute(Qt::WA_Hover);
}

void LyricsScriptButton::setBlurredBackground(const QPixmap &background, const QColor &textColor)
{
    m_background = background;
    m_textColor = textColor;
    update();
}

void LyricsScriptButton::enterEvent(QEnterEvent *event)
{
    m_hovered = true;
    update();
    QPushButton::enterEvent(event);
}

void LyricsScriptButton::leaveEvent(QEvent *event)
{
    m_hovered = false;
    update();
    QPushButton::leaveEvent(event);
}

void LyricsScriptButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    if (!m_background.isNull()) {
        painter.drawPixmap(rect(), m_background);
    } else {
        painter.fillRect(rect(), QColor(20, 22, 30, 150));
    }
    painter.fillRect(rect(), m_hovered ? QColor(255, 255, 255, 42) : QColor(8, 10, 16, 30));
    painter.setPen(QColor(255, 255, 255, m_hovered ? 76 : 48));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));

    QFont buttonFont = font();
    buttonFont.setPointSize(15);
    buttonFont.setWeight(QFont::DemiBold);
    painter.setFont(buttonFont);
    painter.setPen(QColor(0, 0, 0, 105));
    painter.drawText(rect().translated(0, 1), Qt::AlignCenter, text());
    painter.setPen(m_textColor);
    painter.drawText(rect(), Qt::AlignCenter, text());
}

} // namespace PlayerWidgets
