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
#include <QScrollBar>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace PlayerWidgets {

LyricsListWidget::LyricsListWidget(QWidget *parent) : QListWidget(parent)
{
    setUniformItemSizes(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

void LyricsListWidget::updateGeometries()
{
    const int previousPosition = verticalScrollBar()->value();
    QListWidget::updateGeometries();
    if (count() == 0) {
        return;
    }

    const QRect firstRect = visualItemRect(item(0));
    const QRect lastRect = visualItemRect(item(count() - 1));
    if (!firstRect.isValid() || !lastRect.isValid()) {
        return;
    }
    const int offset = verticalScrollBar()->value();
    const int center = viewport()->rect().center().y();
    // 允许首行上方和末行下方留白，避免居中位置被默认的内容边界截断。
    const int minimum = firstRect.center().y() + offset - center;
    const int maximum = lastRect.center().y() + offset - center;
    verticalScrollBar()->setRange(minimum, qMax(minimum, maximum));
    verticalScrollBar()->setValue(previousPosition);
}

void LyricsListWidget::scrollTo(const QModelIndex &index, ScrollHint hint)
{
    if (hint != QAbstractItemView::PositionAtCenter || !index.isValid()) {
        QListWidget::scrollTo(index, hint);
        return;
    }
    // 只处理尚未完成的布局；固定行高后，正常换句无需强制重排整张列表。
    executeDelayedItemsLayout();
    const QRect area = visualRect(index);
    if (area.isValid()) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() + area.center().y() -
                                      viewport()->rect().center().y());
    }
}

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

bool LyricsItemDelegate::setFillProgress(qreal progress)
{
    const qreal bounded = qBound(0.0, progress, 1.0);
    if (qFuzzyCompare(m_fillProgress + 1.0, bounded + 1.0)) {
        return false;
    }
    m_fillProgress = bounded;
    return true;
}

qreal LyricsItemDelegate::wordFillProgress(const Lyrics::Line &line, qint64 position) const
{
    const auto *lyrics = qobject_cast<const QListWidget *>(parent());
    if (lyrics == nullptr || line.words.isEmpty()) {
        return 0.0;
    }
    QFont font = lyrics->font();
    font.setBold(true);
    if (font.pixelSize() > 0) {
        font.setPixelSize(qRound(font.pixelSize() * 1.35));
    } else {
        font.setPointSizeF(font.pointSizeF() * 1.35);
    }
    const QString key = font.toString() + QLatin1Char('\n') + line.text;
    const WordLayout *cached = m_wordLayouts.object(key);
    if (cached == nullptr) {
        QTextLayout layout(line.text, font);
        layout.beginLayout();
        QTextLine shapedLine = layout.createLine();
        if (!shapedLine.isValid()) {
            layout.endLayout();
            return 0.0;
        }
        shapedLine.setLineWidth(qMax(1.0, QFontMetricsF(font).horizontalAdvance(line.text) + 1.0));
        layout.endLayout();
        auto *prepared = new WordLayout;
        prepared->cursorPositions.reserve(line.text.size() + 1);
        for (int cursor = 0; cursor <= line.text.size(); ++cursor) {
            prepared->cursorPositions.append(shapedLine.cursorToX(cursor));
        }
        prepared->textBounds = textOutline(line.text, font).boundingRect();
        m_wordLayouts.insert(key, prepared);
        cached = prepared;
    }
    if (cached->textBounds.width() <= 0.0 || position < line.words.first().startMs) {
        return 0.0;
    }
    if (position >= line.words.last().endMs) {
        return 1.0;
    }
    qreal filledX = cached->textBounds.left();
    for (const Lyrics::Word &word : line.words) {
        const int endCursor = word.textStart + word.textLength;
        if (word.textStart < 0 || endCursor >= cached->cursorPositions.size()) {
            continue;
        }
        if (position < word.startMs) {
            break;
        }
        const qreal startX = cached->cursorPositions.at(word.textStart);
        const qreal endX = cached->cursorPositions.at(endCursor);
        if (position >= word.endMs) {
            filledX = endX;
            continue;
        }
        // 字间停顿保持已填部分，拖音在该字实际排版宽度内缓慢推进。
        const qreal progress = static_cast<qreal>(position - word.startMs) /
                               qMax(qint64(1), word.endMs - word.startMs);
        filledX = startX + (endX - startX) * progress;
        break;
    }
    return qBound(0.0, (filledX - cached->textBounds.left()) / cached->textBounds.width(), 1.0);
}

void LyricsItemDelegate::prepareLine(const Lyrics::Line &line) const
{
    const auto *lyrics = qobject_cast<const QListWidget *>(parent());
    if (lyrics == nullptr) {
        return;
    }
    QFont font = lyrics->font();
    font.setBold(false);
    const QString text = line.text.isEmpty() ? QStringLiteral("···") : line.text;
    const QString inactiveText = QFontMetrics(font).elidedText(
        text, Qt::ElideRight, qMax(0, lyrics->viewport()->width() - 32));
    textOutline(inactiveText, font);
    font.setBold(true);
    if (font.pixelSize() > 0) {
        font.setPixelSize(qRound(font.pixelSize() * 1.35));
    } else {
        font.setPointSizeF(font.pointSizeF() * 1.35);
    }
    textOutline(text, font);
    if (!line.words.isEmpty()) {
        wordFillProgress(line, line.startMs);
    }
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
    m_activeColors.clear();

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

    if (active) {
        const QList<QColor> colors = activeLyricsColors(font);
        painter->fillPath(textPath, colors.at(0));
        if (m_fillProgress > 0.0) {
            // 按完整字形宽度填色，裁剪与横向滚动共用坐标，不能按可见部分重新计算进度。
            painter->save();
            const QRectF filledArea(bounds.left(), bounds.top() - 1.0,
                                     bounds.width() * m_fillProgress, bounds.height() + 2.0);
            painter->setClipRect(filledArea, Qt::IntersectClip);
            painter->fillPath(textPath, colors.at(1));
            painter->restore();
        }
    } else {
        const bool locallyDark = backgroundLuminance(textArea.toAlignedRect()) < 0.52;
        painter->fillPath(textPath,
                          locallyDark ? QColor(248, 249, 255, 225) : QColor(17, 19, 27, 225));
    }
    painter->restore();
}

QSize LyricsItemDelegate::sizeHint(const QStyleOptionViewItem &option,
                                   const QModelIndex &) const
{
    QFont font = option.font;
    font.setBold(true);
    if (font.pixelSize() > 0) {
        font.setPixelSize(qRound(font.pixelSize() * 1.35));
    } else {
        font.setPointSizeF(font.pointSizeF() * 1.35);
    }
    // 所有行都为高亮字号预留空间，换句只改变绘制，不改变行高和后续行的位置。
    return QSize(1, qMax(68, QFontMetrics(font).height() + 12));
}

QRect LyricsItemDelegate::backgroundSampleRect(const QRect &itemRect) const
{
    const auto *lyrics = qobject_cast<const QListWidget *>(parent());
    if (lyrics == nullptr || m_background.isNull()) {
        return {};
    }

    const QWidget *surface = lyrics;
    while (surface->parentWidget() != nullptr &&
           surface->objectName() != QStringLiteral("appSurface")) {
        surface = surface->parentWidget();
    }
    if (surface->objectName() != QStringLiteral("appSurface") || surface->size().isEmpty()) {
        return {};
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
        m_activeColors.clear();
    }

    const QPoint surfacePosition =
        lyrics->viewport()->mapTo(const_cast<QWidget *>(surface), itemRect.topLeft());
    const qreal scaleX = static_cast<qreal>(m_sampledBackground.width()) / surface->width();
    const qreal scaleY = static_cast<qreal>(m_sampledBackground.height()) / surface->height();
    const QRect sampleRect = QRectF(surfacePosition.x() * scaleX, surfacePosition.y() * scaleY,
                                    itemRect.width() * scaleX, itemRect.height() * scaleY)
                                 .toAlignedRect();
    return sampleRect.intersected(m_sampledBackground.rect());
}

qreal LyricsItemDelegate::backgroundLuminance(const QRect &itemRect) const
{
    const QRect sampleRect = backgroundSampleRect(itemRect);
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

QList<QColor> LyricsItemDelegate::activeLyricsColors(const QFont &font) const
{
    const auto *lyrics = qobject_cast<const QListWidget *>(parent());
    QRect band(0, 0, 1, 1);
    if (lyrics != nullptr) {
        band = lyrics->viewport()->rect().adjusted(16, 0, -16, 0);
        const int textHeight = qMax(68, QFontMetrics(font).height() + 8);
        band.setTop(lyrics->viewport()->rect().center().y() - textHeight / 2);
        band.setHeight(textHeight);
        band.setWidth(qMax(1, band.width()));
    }
    const QRect sampleRect = backgroundSampleRect(band);
    const QString fontKey = font.toString();
    if (m_activeColors.isEmpty() || m_activeSampleRect != sampleRect ||
        m_activeFontKey != fontKey || m_activeBandSize != band.size()) {
        const auto luminance = [](const QColor &color) {
            const auto linear = [](qreal channel) {
                return channel <= 0.04045 ? channel / 12.92
                                          : std::pow((channel + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
                   0.0722 * linear(color.blueF());
        };
        qreal totalLuminance = 0.0;
        int samples = 0;
        for (int y = sampleRect.top(); y <= sampleRect.bottom(); ++y) {
            for (int x = sampleRect.left(); x <= sampleRect.right(); ++x) {
                totalLuminance += luminance(m_sampledBackground.pixelColor(x, y));
                ++samples;
            }
        }
        const qreal background = samples > 0 ? totalLuminance / samples
                                             : luminance(QColor(QStringLiteral("#0b0d14")));
        // 整句使用统一底色和高亮色，以色相差异明确区分填色前后，并兼顾背景对比度。
        const QList<QList<QColor>> palettes = {
            {QColor(QStringLiteral("#edf0f7")), QColor(QStringLiteral("#76dcff"))},
            {QColor(QStringLiteral("#303746")), QColor(QStringLiteral("#6326ad"))}};
        qreal bestContrast = -1.0;
        for (const auto &palette : palettes) {
            qreal minimumContrast = 21.0;
            for (const QColor &color : palette) {
                const qreal text = luminance(color);
                const qreal contrast = (qMax(background, text) + 0.05) /
                                       (qMin(background, text) + 0.05);
                minimumContrast = qMin(minimumContrast, contrast);
            }
            if (minimumContrast > bestContrast) {
                bestContrast = minimumContrast;
                m_activeColors = palette;
            }
        }
        m_activeSampleRect = sampleRect;
        m_activeFontKey = fontKey;
        m_activeBandSize = band.size();
    }

    return m_activeColors;
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
