#ifndef DESKTOP_LYRICS_WINDOW_H
#define DESKTOP_LYRICS_WINDOW_H

#include "lyric_parser.h"

#include <QFont>
#include <QColor>
#include <QIcon>
#include <QPainterPath>
#include <QPixmap>
#include <QWidget>

#include <array>

class QCloseEvent;
class QEnterEvent;
class QMouseEvent;
class QMoveEvent;
class QMenu;
class QPushButton;
class QResizeEvent;
class QTimer;

class DesktopLyricsWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit DesktopLyricsWindow();
    void restoreConfiguration(const QString &configuration, const QPoint &preferredScreen);
    QString configuration() const;
    void setLyricsFont(const QFont &font);
    void setPlaybackControls(const QIcon &playIcon, const QString &playTip,
                             const QIcon &modeIcon, const QString &modeTip, bool enabled);
    void setLines(const Lyrics::Line &current, const QString &next, qreal progress,
                  qreal scrollProgress = 0.0, int currentSlot = 0);
    void showLyrics();
    void flushConfiguration();

signals:
    void configurationChanged(const QString &configuration);
    void visibilityChanged(bool visible);
    void previousRequested();
    void playPauseRequested();
    void nextRequested();
    void playModeRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void keepOnScreen();
    void scheduleSave();
    void updateOrientation();
    void rebuildPaths();
    void rebuildImages(const QColor &base, const QColor &played, qreal devicePixelRatio);
    void showColorMenu(QPushButton *button);
    void layoutToolbar();
    QRectF lyricsArea() const;
    QRectF lineArea(int slot) const;
    Qt::Edges edgesAt(const QPoint &position) const;
    static QPainterPath lyricPath(const QString &text, const QFont &font, bool vertical);

    QWidget *m_toolbar = nullptr;
    QWidget *m_playbackControls = nullptr;
    QPushButton *m_previousButton = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QPushButton *m_modeButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    int m_toolbarHeight = 40;
    QTimer *m_saveTimer = nullptr;
    QMenu *m_colorMenu = nullptr;
    QFont m_lyricsFont;
    Lyrics::Line m_current;
    QString m_next;
    int m_currentSlot = 0;
    qreal m_characterWidth = 0.0;
    qreal m_characterHeight = 0.0;
    qreal m_lineHeight = 0.0;
    QPainterPath m_currentPath;
    QPainterPath m_nextPath;
    std::array<QPixmap, 2> m_baseImages;
    QPixmap m_playedImage;
    QColor m_cachedBase;
    QColor m_cachedPlayed;
    qreal m_cachedDevicePixelRatio = 0.0;
    bool m_imagesDirty = true;
    qreal m_progress = 0.0;
    qreal m_scrollProgress = 0.0;
    QColor m_defaultBase{QStringLiteral("#f2f4f8")};
    QColor m_defaultPlayed{QStringLiteral("#38bdf8")};
    int m_colorIndex = -1;
    bool m_vertical = false;
    bool m_hovered = false;
    bool m_restoring = false;
    bool m_dragging = false;
    Qt::Edges m_resizeEdges;
    QPoint m_dragOrigin;
    QRect m_initialGeometry;
};

#endif // DESKTOP_LYRICS_WINDOW_H
