#ifndef PLAYER_WIDGETS_H
#define PLAYER_WIDGETS_H

#include "music_types.h"
#include "lyric_parser.h"

#include <QAbstractTableModel>
#include <QCache>
#include <QColor>
#include <QElapsedTimer>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>

#include <functional>

class QLineEdit;
class QMediaMetaData;
class AudioPlayer;
class QMenu;

namespace PlayerWidgets {

QImage loadScaledImage(const QString &path, const QSize &bounds);
QImage backgroundSample(const QString &path);
void cacheBackgroundSample(const QString &path, const QImage &image);
inline const QString kDefaultBackgroundPath = QStringLiteral(":/backgrounds/default.png");

QString styleSheetWithFontSizes(const QString &baseStyleSheet, int applicationFontSize,
                                int lyricsFontSize);

QString artistFromMetadata(const QMediaMetaData &metadata);

void showIconButtonToolTip(QPushButton *button, bool refresh = false);

void configureIconButton(QPushButton *button, const QString &iconPath, const QString &toolTip,
                         const QSize &iconSize = QSize(20, 20));

QIcon tintedIcon(const QString &path, const QSize &size, const QColor &color,
                 qreal devicePixelRatio);

class PlaybackProgressSlider final : public QSlider
{
public:
    explicit PlaybackProgressSlider(QWidget *parent);

protected:
    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void setPositionFromPoint(const QPoint &position);

    int m_dragOffset = 0;
};

class EmptyGroupEditorDismissFilter final : public QObject
{
public:
    explicit EmptyGroupEditorDismissFilter(QLineEdit *editor);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QLineEdit *m_editor;
};

class SongTableModel final : public QAbstractTableModel
{
public:
    SongTableModel(QList<Song> &songs, const QHash<QString, qint64> &fileSizes,
                   QObject *parent);

    int rowCount(const QModelIndex &parent = {}) const override;

    int columnCount(const QModelIndex &parent = {}) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setSongs(QList<Song> songs);

    void refreshFileSizes();

    void refreshArtist(int row);

private:
    // MainWindow owns these collections; the model exposes them without per-cell objects.
    QList<Song> &m_songs;
    const QHash<QString, qint64> &m_fileSizes;
};

class SongTableView final : public QTableView
{
public:
    explicit SongTableView(QWidget *parent = nullptr);

    int hoveredRow() const;

protected:
    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void leaveEvent(QEvent *event) override;

    void paintEvent(QPaintEvent *event) override;

private:
    int m_hoveredRow = -1;
};

class SongTableItemDelegate final : public QStyledItemDelegate
{
public:
    explicit SongTableItemDelegate(QTableView *table);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
};

class QueueItemDelegate final : public QStyledItemDelegate
{
public:
    QueueItemDelegate(QListWidget *queue, AudioPlayer *player);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view,
                   const QStyleOptionViewItem &option, const QModelIndex &index) override;

private:
    AudioPlayer *m_player;
};

class PopupToggleEventFilter final : public QObject
{
public:
    PopupToggleEventFilter(QPushButton *button, QMenu *menu);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPushButton *m_button;
    QMenu *m_menu;
};

class LyricsListWidget final : public QListWidget
{
public:
    explicit LyricsListWidget(QWidget *parent = nullptr);

    void scrollTo(const QModelIndex &index, ScrollHint hint = EnsureVisible) override;

protected:
    void updateGeometries() override;
};

class LyricsItemDelegate final : public QStyledItemDelegate
{
public:
    explicit LyricsItemDelegate(QObject *parent = nullptr);

    bool setScrollProgress(qreal progress);

    bool setFillProgress(qreal progress);

    qreal wordFillProgress(const Lyrics::Line &line, qint64 position) const;

    void prepareLine(const Lyrics::Line &line) const;

    bool needsHorizontalScroll(const QModelIndex &index, int width) const;

    void setBackground(const QString &path);

    QList<QColor> displayColors() const;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    qreal backgroundLuminance(const QRect &itemRect) const;

    QRect backgroundSampleRect(const QRect &itemRect) const;

    QList<QColor> activeLyricsColors(const QFont &font) const;

    QPainterPath textOutline(const QString &text, const QFont &font) const;

    mutable QCache<QString, QPainterPath> m_textOutlines{1024 * 1024};
    qreal m_scrollProgress = 0.0;
    qreal m_fillProgress = 0.0;
    struct WordLayout
    {
        QList<qreal> cursorPositions;
        QRectF textBounds;
    };

    // 保留多句排版，预热下一句不会驱逐当前句，往回跳转也可复用。
    mutable QCache<QString, WordLayout> m_wordLayouts{8};
    QList<QColor> m_colors;
    bool m_darkBackground = true;
    QImage m_background;
    mutable QImage m_sampledBackground;
    mutable QSize m_sampledSurfaceSize;
    mutable QRect m_activeSampleRect;
    mutable QString m_activeFontKey;
    mutable QSize m_activeBandSize;
    mutable QList<QColor> m_activeColors;
};

class ImmediateToolTipFilter final : public QObject
{
public:
    explicit ImmediateToolTipFilter(QPushButton *button);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPushButton *m_button;
};

class FramelessWindowEventFilter final : public QObject
{
public:
    FramelessWindowEventFilter(QWidget *window, QWidget *titleBar);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *m_window;
    QWidget *m_titleBar;
};

class TraySongLabel final : public QLabel
{
public:
    explicit TraySongLabel(QWidget *parent);

    void setSongName(const QString &songName);

protected:
    void showEvent(QShowEvent *event) override;

    void hideEvent(QHideEvent *event) override;

    void resizeEvent(QResizeEvent *event) override;

    void changeEvent(QEvent *event) override;

    void paintEvent(QPaintEvent *event) override;

private:
    void restartScroll();

    static constexpr qint64 EndpointPauseMs = 1000;
    static constexpr int ScrollPixelsPerSecond = 30;
    QTimer m_scrollTimer;
    QElapsedTimer m_scrollElapsed;
    int m_textWidth = 0;
};

class TrayPlaybackPanel final : public QWidget
{
public:
    explicit TrayPlaybackPanel(QWidget *parent);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QPixmap m_background;
};

class CaptionIconHoverFilter final : public QObject
{
public:
    explicit CaptionIconHoverFilter(QPushButton *button);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPushButton *m_button;
};

class LyricsScriptButton final : public QPushButton
{
public:
    explicit LyricsScriptButton(QWidget *parent = nullptr);

    void setBlurredBackground(const QPixmap &background, const QColor &textColor);

protected:
    void enterEvent(QEnterEvent *event) override;

    void leaveEvent(QEvent *event) override;

    void paintEvent(QPaintEvent *) override;

private:
    QPixmap m_background;
    QColor m_textColor = Qt::white;
    bool m_hovered = false;
};

class QueueDrawer final : public QWidget
{
public:
    explicit QueueDrawer(QWidget *parent = nullptr);

    void setBackdropProvider(const std::function<QPixmap(const QWidget *)> &provider);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    std::function<QPixmap(const QWidget *)> m_backdropProvider;
};

class AppSurface final : public QWidget
{
public:
    explicit AppSurface(QWidget *parent = nullptr);

    bool setBackground(const QString &path);

    void setBlurTargets(QWidget *content, QWidget *playerBar);

    void setContentBlurred(bool blurred);

    QPixmap blurredRegion(const QWidget *target) const;

protected:
    void paintEvent(QPaintEvent *) override;

    void resizeEvent(QResizeEvent *event) override;

private:
    void rebuildBlurredBackground();

    void updateBackgroundTone();

    QRectF blurredSourceRect(const QRect &area) const;

    QTimer m_backgroundResizeTimer;
    QString m_backgroundPath;
    quint64 m_backgroundGeneration = 0;
    QPixmap m_sharpBackground;
    QPixmap m_blurredBackground;
    QColor m_edgeColor = QColor(QStringLiteral("#0b0d14"));
    bool m_darkBackground = true;
    bool m_contentBlurred = false;
    QWidget *m_content = nullptr;
    QWidget *m_playerBar = nullptr;
};

} // namespace PlayerWidgets

#endif // PLAYER_WIDGETS_H
