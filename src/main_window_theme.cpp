#include "main_window.h"
#include "player_widgets.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVariant>

#include <utility>

using namespace PlayerWidgets;

void MainWindow::updateLyricsPalette(const QString &backgroundPath)
{
    auto *delegate = static_cast<LyricsItemDelegate *>(m_lyrics->itemDelegate());
    delegate->setBackground(backgroundPath);
    m_lyrics->viewport()->update();
}

void MainWindow::updateLyricsScriptButtonPalette(const QString &backgroundPath)
{
    if (m_lyricsScriptButton == nullptr) {
        return;
    }

    qreal localLuminance = 0.12;
    const QImage background = backgroundSample(backgroundPath);
    if (!background.isNull() && centralWidget() != nullptr && !centralWidget()->size().isEmpty()) {
        const QSize sampleSize(qMax(1, centralWidget()->width() / 6),
                               qMax(1, centralWidget()->height() / 6));
        const QImage scaled =
            background.scaled(sampleSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const QPoint cropOrigin((scaled.width() - sampleSize.width()) / 2,
                                (scaled.height() - sampleSize.height()) / 2);
        const QPoint buttonPosition = m_lyricsScriptButton->mapTo(centralWidget(), QPoint(0, 0));
        QRect sampleRect(qRound(buttonPosition.x() / 6.0) + cropOrigin.x(),
                         qRound(buttonPosition.y() / 6.0) + cropOrigin.y(),
                         qMax(1, qRound(m_lyricsScriptButton->width() / 6.0)),
                         qMax(1, qRound(m_lyricsScriptButton->height() / 6.0)));
        sampleRect = sampleRect.intersected(scaled.rect());
        if (!sampleRect.isEmpty()) {
            qreal red = 0.0;
            qreal green = 0.0;
            qreal blue = 0.0;
            int samples = 0;
            for (int y = sampleRect.top(); y <= sampleRect.bottom(); ++y) {
                for (int x = sampleRect.left(); x <= sampleRect.right(); ++x) {
                    const QColor color = scaled.pixelColor(x, y);
                    red += color.redF();
                    green += color.greenF();
                    blue += color.blueF();
                    ++samples;
                }
            }
            const QColor localColor =
                QColor::fromRgbF(red / samples, green / samples, blue / samples);
            localLuminance = (0.2126 * localColor.redF()) + (0.7152 * localColor.greenF()) +
                             (0.0722 * localColor.blueF());
        }
    }

    const QColor textColor = localLuminance > 0.58 ? QColor(QStringLiteral("#101218"))
                                                   : QColor(QStringLiteral("#ffffff"));
    auto *button = static_cast<LyricsScriptButton *>(m_lyricsScriptButton);
    const auto *surface = static_cast<AppSurface *>(centralWidget());
    button->setBlurredBackground(surface->blurredRegion(button), textColor);
}

void MainWindow::updateCaptionButtonPalette(const QString &backgroundPath)
{
    if (centralWidget() == nullptr || centralWidget()->size().isEmpty()) {
        return;
    }

    const QImage background = backgroundSample(backgroundPath);
    QImage scaledBackground;
    QPoint cropOrigin;
    if (!background.isNull()) {
        scaledBackground = background.scaled(
            centralWidget()->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        cropOrigin = QPoint((scaledBackground.width() - centralWidget()->width()) / 2,
                            (scaledBackground.height() - centralWidget()->height()) / 2);
    }

    for (QPushButton *button : std::as_const(m_captionButtons)) {
        if (button == nullptr) {
            continue;
        }
        qreal luminance = 0.18;
        if (!scaledBackground.isNull()) {
            const QPoint position = button->mapTo(centralWidget(), QPoint(0, 0));
            QRect sampleRect(position + cropOrigin, button->size());
            sampleRect = sampleRect.intersected(scaledBackground.rect());
            if (!sampleRect.isEmpty()) {
                qreal luminanceSum = 0.0;
                int pixelCount = 0;
                for (int y = sampleRect.top(); y <= sampleRect.bottom(); ++y) {
                    for (int x = sampleRect.left(); x <= sampleRect.right(); ++x) {
                        const QColor pixel = scaledBackground.pixelColor(x, y);
                        luminanceSum += 0.2126 * pixel.redF() + 0.7152 * pixel.greenF() +
                                        0.0722 * pixel.blueF();
                        ++pixelCount;
                    }
                }
                luminance = luminanceSum / pixelCount;
            }
        }
        const QColor iconColor = luminance > 0.52 ? QColor(QStringLiteral("#171923"))
                                                  : QColor(QStringLiteral("#f5f7ff"));
        const QString path = button->property("iconPath").toString();
        const QSize iconSize(button->property("iconWidth").toInt(),
                             button->property("iconHeight").toInt());
        button->setProperty("normalIconColor", iconColor);
        button->setIcon(tintedIcon(path, iconSize, iconColor, button->devicePixelRatioF()));
    }
}

void MainWindow::updateDialogTheme(const QString &backgroundPath)
{
    const QImage background = backgroundSample(backgroundPath);
    if (background.isNull()) {
        m_dialogThemeStyle.clear();
    } else {
        const QImage sample =
            background.scaled(QSize(48, 48), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_RGB32);
        qint64 red = 0;
        qint64 green = 0;
        qint64 blue = 0;
        const int pixelCount = sample.width() * sample.height();
        for (int y = 0; y < sample.height(); ++y) {
            const QRgb *scanLine = reinterpret_cast<const QRgb *>(sample.constScanLine(y));
            for (int x = 0; x < sample.width(); ++x) {
                red += qRed(scanLine[x]);
                green += qGreen(scanLine[x]);
                blue += qBlue(scanLine[x]);
            }
        }
        QColor sourceColor(static_cast<int>(red / pixelCount), static_cast<int>(green / pixelCount),
                           static_cast<int>(blue / pixelCount));
        const qreal luminance = (0.2126 * sourceColor.redF()) + (0.7152 * sourceColor.greenF()) +
                                (0.0722 * sourceColor.blueF());
        const bool useDarkText = luminance > 0.58;
        const auto blend = [](const QColor &source, const QColor &target, qreal targetWeight) {
            return QColor(
                qRound(source.red() * (1.0 - targetWeight) + target.red() * targetWeight),
                qRound(source.green() * (1.0 - targetWeight) + target.green() * targetWeight),
                qRound(source.blue() * (1.0 - targetWeight) + target.blue() * targetWeight));
        };
        const QColor surface =
            blend(sourceColor, useDarkText ? QColor(250, 250, 252) : QColor(12, 14, 21), 0.84);
        const QColor field =
            blend(sourceColor, useDarkText ? QColor(238, 240, 245) : QColor(29, 33, 46), 0.78);
        const QColor border =
            blend(sourceColor, useDarkText ? QColor(110, 115, 128) : QColor(103, 110, 137), 0.62);
        QColor accent = sourceColor.toHsl();
        accent.setHsl(accent.hslHue() < 0 ? 250 : accent.hslHue(),
                      qBound(105, accent.hslSaturation(), 190), useDarkText ? 105 : 155);
        const QColor text = useDarkText ? QColor(31, 34, 43) : QColor(235, 237, 245);
        const QColor mutedText = useDarkText ? QColor(76, 80, 92) : QColor(179, 184, 202);
        const QColor selectedText =
            accent.lightness() > 145 ? QColor(24, 26, 34) : QColor(255, 255, 255);
        m_dialogThemeStyle = QStringLiteral(R"(
QDialog, QMessageBox, QInputDialog, QFileDialog {
    color: %1;
    background-color: %2;
}
QDialog QLabel, QMessageBox QLabel, QInputDialog QLabel, QFileDialog QLabel {
    color: %1;
    background-color: transparent;
}
QDialog QLineEdit, QDialog QComboBox, QDialog QSpinBox, QDialog QPlainTextEdit {
    color: %1;
    background-color: %3;
    border-color: %4;
}
#metadataDialog #metadataValueEditor, #metadataDialog #metadataTable {
    color: %1;
    background-color: %3;
    border-color: %4;
}
#metadataDialog #metadataTable QLineEdit {
    min-height: 0;
    padding: 0 6px;
    color: %1;
    background-color: %3;
    border: 1px solid %5;
    border-radius: 0;
    selection-color: %7;
    selection-background-color: %5;
}
#metadataDialog #metadataSummary {
    color: %6;
}
#groupMembershipDialog #groupMembershipCount,
#groupMembershipDialog #groupMembershipEmpty,
#lyricsSelectionDialog #dialogDescription {
    color: %6;
}
#groupMembershipDialog #groupMembershipList::indicator {
    background-color: transparent;
    border-color: %4;
}
#groupMembershipDialog #groupMembershipList::indicator:checked {
    background-color: %5;
    border-color: %5;
}
#lyricsSelectionDialog #lyricsResultList,
#lyricsSelectionDialog #lyricsPreview {
    color: %1;
    background-color: %3;
    border-color: %4;
}
QDialog QPushButton#primaryDialogButton {
    color: %7;
    background-color: %5;
    border-color: %5;
}
QDialog QPushButton#secondaryDialogButton {
    color: %1;
    background-color: transparent;
    border-color: %4;
}
QDialog QLineEdit:focus, QDialog QComboBox:focus, QDialog QSpinBox:focus,
QDialog QPlainTextEdit:focus {
    border-color: %5;
}
QDialog QListWidget, QDialog QTableWidget, QFileDialog QTreeView, QFileDialog QListView {
    color: %1;
    background-color: %3;
    alternate-background-color: %2;
    border-color: %4;
}
QDialog QHeaderView, QFileDialog QHeaderView,
QDialog QHeaderView::section, QFileDialog QHeaderView::section {
    color: %6;
    background-color: %2;
}
QDialog QDialogButtonBox QPushButton, QMessageBox QPushButton,
QFileDialog QPushButton, QFileDialog QToolButton {
    color: %1;
    background-color: %3;
    border-color: %4;
}
QDialog QDialogButtonBox QPushButton:hover, QMessageBox QPushButton:hover,
QFileDialog QPushButton:hover, QFileDialog QToolButton:hover {
    color: %7;
    background-color: %5;
    border-color: %5;
}
QFileDialog QComboBox QAbstractItemView, QInputDialog QComboBox QAbstractItemView {
    color: %1;
    background-color: %3;
    selection-color: %7;
    selection-background-color: %5;
}
)")
                                 .arg(text.name(), surface.name(), field.name(), border.name(),
                                      accent.name(), mutedText.name(), selectedText.name());
    }

    const int applicationFontSize = qMax(1, QApplication::font().pixelSize());
    const int lyricsFontSize =
        m_lyrics == nullptr ? applicationFontSize : qMax(1, m_lyrics->font().pixelSize());
    qApp->setStyleSheet(
        styleSheetWithFontSizes(m_baseStyleSheet, applicationFontSize, lyricsFontSize) +
        m_dialogThemeStyle);
}

void MainWindow::updateSongInfoPalette(const QString &backgroundPath)
{
    const QImage background = backgroundSample(backgroundPath);
    const auto applyAdaptiveText = [this, &background](QLabel *label, const QColor &darkText,
                                                       const QColor &lightText) {
        bool locallyDark = true;
        if (!background.isNull() && !centralWidget()->size().isEmpty()) {
            const QSize sampleSize(qMax(1, centralWidget()->width() / 8),
                                   qMax(1, centralWidget()->height() / 8));
            const QImage scaled = background.scaled(sampleSize, Qt::KeepAspectRatioByExpanding,
                                                    Qt::SmoothTransformation);
            const QPoint cropOrigin((scaled.width() - sampleSize.width()) / 2,
                                    (scaled.height() - sampleSize.height()) / 2);
            const QPoint labelPosition = label->mapTo(centralWidget(), QPoint(0, 0));
            QRect sampleRect(qRound(labelPosition.x() / 8.0) + cropOrigin.x(),
                             qRound(labelPosition.y() / 8.0) + cropOrigin.y(),
                             qMax(1, qRound(label->width() / 8.0)),
                             qMax(1, qRound(label->height() / 8.0)));
            sampleRect = sampleRect.intersected(scaled.rect());

            qreal luminance = 0.0;
            int samples = 0;
            for (int y = sampleRect.top(); y <= sampleRect.bottom(); ++y) {
                for (int x = sampleRect.left(); x <= sampleRect.right(); ++x) {
                    const QColor color = scaled.pixelColor(x, y);
                    luminance +=
                        0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF();
                    ++samples;
                }
            }
            locallyDark = samples == 0 || luminance / samples < 0.52;
        }

        label->setStyleSheet(
            QStringLiteral("color: %1;").arg((locallyDark ? lightText : darkText).name()));
        label->setGraphicsEffect(nullptr);
    };

    applyAdaptiveText(m_detailTitle, QColor(QStringLiteral("#11131b")), Qt::white);
    applyAdaptiveText(m_artist, QColor(QStringLiteral("#303440")),
                      QColor(QStringLiteral("#e2e5ee")));
    applyAdaptiveText(m_album, QColor(QStringLiteral("#555b68")),
                      QColor(QStringLiteral("#c4c8d4")));
}
