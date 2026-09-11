#include "player_widgets.h"
#include "music_file_utils.h"

#include <QEvent>
#include <QHelpEvent>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListWidget>
#include "audio_player.h"
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolTip>
#include <QUrl>
#include <QVariant>

#include <utility>

namespace PlayerWidgets {

SongTableModel::SongTableModel(QList<Song> &songs, const QHash<QString, qint64> &fileSizes,
                               QObject *parent)
    : QAbstractTableModel(parent), m_songs(songs), m_fileSizes(fileSizes)
{
}

int SongTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_songs.size());
}

int SongTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : 3;
}

QVariant SongTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_songs.size() ||
        index.column() < 0 || index.column() >= 3) {
        return {};
    }
    const Song &song = m_songs.at(index.row());
    if (role == Qt::UserRole) {
        return song.id;
    }
    if (role == Qt::TextAlignmentRole) {
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (role == Qt::ToolTipRole && index.column() == 0) {
        return song.title;
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    switch (index.column()) {
    case 0:
        return song.title;
    case 1:
        return song.artist.isEmpty() ? QStringLiteral("未知歌手") : song.artist;
    case 2: {
        const qint64 bytes = m_fileSizes.value(normalizedPath(song.path), -1);
        return bytes < 0 ? QStringLiteral("--")
                         : QString::number(bytes / (1024.0 * 1024.0), 'f', 2) +
                               QStringLiteral(" MB");
    }
    default:
        return {};
    }
}

QVariant SongTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    static const QStringList labels = {QStringLiteral("歌曲"), QStringLiteral("歌手"),
                                       QStringLiteral("文件大小")};
    return section >= 0 && section < labels.size() ? QVariant(labels.at(section)) : QVariant();
}

void SongTableModel::setSongs(QList<Song> songs)
{
    beginResetModel();
    m_songs = std::move(songs);
    endResetModel();
}

void SongTableModel::refreshFileSizes()
{
    if (!m_songs.isEmpty()) {
        emit dataChanged(index(0, 2), index(rowCount() - 1, 2), {Qt::DisplayRole});
    }
}

void SongTableModel::refreshArtist(int row)
{
    if (row >= 0 && row < rowCount()) {
        emit dataChanged(index(row, 1), index(row, 1), {Qt::DisplayRole});
    }
}

EmptyGroupEditorDismissFilter::EmptyGroupEditorDismissFilter(QLineEdit *editor)
    : QObject(editor), m_editor(editor)
{
}

bool EmptyGroupEditorDismissFilter::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::MouseButtonPress) {
        return QObject::eventFilter(watched, event);
    }
    QWidget *clickedWidget = qobject_cast<QWidget *>(watched);
    if (clickedWidget != nullptr && clickedWidget != m_editor &&
        !m_editor->isAncestorOf(clickedWidget)) {
        QMetaObject::invokeMethod(m_editor, "editingFinished", Qt::QueuedConnection);
    }
    return QObject::eventFilter(watched, event);
}

SongTableView::SongTableView(QWidget *parent) : QTableView(parent)
{
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
}

int SongTableView::hoveredRow() const
{
    return m_hoveredRow;
}

void SongTableView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        const QModelIndex index = indexAt(event->position().toPoint());
        if (index.isValid()) {
            // 右键只更新选中行，避免默认处理将底部未完整显示的行滚入视口。
            selectionModel()->select(index, QItemSelectionModel::ClearAndSelect |
                                                QItemSelectionModel::Rows);
        }
        event->accept();
        return;
    }
    QTableView::mousePressEvent(event);
}

void SongTableView::mouseMoveEvent(QMouseEvent *event)
{
    const int row = indexAt(event->position().toPoint()).row();
    if (row != m_hoveredRow) {
        const int previous = m_hoveredRow;
        m_hoveredRow = row;
        for (int changed : {previous, row}) {
            if (changed >= 0) {
                QRect area = visualRect(model()->index(changed, 0));
                area.setLeft(0);
                area.setRight(viewport()->width() - 1);
                viewport()->update(area);
            }
        }
    }
    QTableView::mouseMoveEvent(event);
}

void SongTableView::leaveEvent(QEvent *event)
{
    if (m_hoveredRow >= 0) {
        m_hoveredRow = -1;
        viewport()->update();
    }
    QTableView::leaveEvent(event);
}

void SongTableView::paintEvent(QPaintEvent *event)
{
    QTableView::paintEvent(event);
    QPainter painter(viewport());
    const QModelIndexList selectedRows = selectionModel()->selectedRows();
    for (const QModelIndex &selectedRow : selectedRows) {
        QRect selectedRect = visualRect(model()->index(selectedRow.row(), 0));
        if (!selectedRect.isValid()) {
            continue;
        }
        selectedRect.setLeft(0);
        selectedRect.setRight(viewport()->width() - 1);
        painter.fillRect(selectedRect, QColor(255, 255, 255, 26));
    }

    if (m_hoveredRow < 0 || selectionModel()->isRowSelected(m_hoveredRow, QModelIndex())) {
        return;
    }

    QRect rowRect = visualRect(model()->index(m_hoveredRow, 0));
    if (!rowRect.isValid()) {
        return;
    }
    rowRect.setLeft(0);
    rowRect.setRight(viewport()->width() - 1);
    painter.fillRect(rowRect, QColor(255, 255, 255, 16));
}

SongTableItemDelegate::SongTableItemDelegate(QTableView *table) : QStyledItemDelegate(table)
{
}

void SongTableItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const
{
    QStyleOptionViewItem rowOption(option);
    rowOption.state &= ~QStyle::State_MouseOver;
    rowOption.state &= ~QStyle::State_Selected;
    rowOption.state &= ~QStyle::State_HasFocus;
    const auto *table = qobject_cast<QTableView *>(parent());
    const bool currentSong =
        table != nullptr && index.column() == 0 && index.data(Qt::UserRole).toLongLong() > 0 &&
        index.data(Qt::UserRole).toLongLong() == table->property("playingSongId").toLongLong();
    if (index.column() == 0) {
        rowOption.rect.adjust(20, 0, 0, 0);
    }
    QStyledItemDelegate::paint(painter, rowOption, index);
    if (currentSong) {
        painter->save();
        const QIcon icon(table->property("songIsPlaying").toBool()
                             ? QStringLiteral(":/icons/pause.svg")
                             : QStringLiteral(":/icons/play.svg"));
        const QRect iconRect(option.rect.left() + 2, option.rect.center().y() - 8, 16, 16);
        icon.paint(painter, iconRect, Qt::AlignCenter);
        painter->restore();
    }
}

QueueItemDelegate::QueueItemDelegate(QListWidget *queue, AudioPlayer *player)
    : QStyledItemDelegate(queue), m_player(player)
{
}

bool QueueItemDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view,
                                  const QStyleOptionViewItem &option,
                                  const QModelIndex &index)
{
    if (event == nullptr || view == nullptr || !index.isValid() ||
        event->type() != QEvent::ToolTip) {
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }

    QStyleOptionViewItem rowOption(option);
    initStyleOption(&rowOption, index);
    const QUrl source = m_player->source();
    const bool currentSong =
        source.isLocalFile() &&
        normalizedPath(source.toLocalFile()) == normalizedPath(index.data(Qt::UserRole).toString());
    QRect textRect;
    // 与实际绘制保持一致：播放行使用粗体和固定留白，普通行由样式决定文本区域。
    if (currentSong) {
        rowOption.font.setBold(true);
        textRect = option.rect.adjusted(12, 0, -12, 0);
    } else {
        textRect = view->style()->subElementRect(QStyle::SE_ItemViewItemText, &rowOption, view);
    }
    const QString displayedText = QFontMetrics(rowOption.font).elidedText(
        rowOption.text, Qt::ElideRight, qMax(0, textRect.width()));
    if (displayedText == rowOption.text) {
        QToolTip::hideText();
        return true;
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

void QueueItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    const QUrl source = m_player->source();
    // 以实际音源匹配列表项，选择其他行或重新加载列表不会改变播放标记。
    const bool currentSong =
        source.isLocalFile() &&
        normalizedPath(source.toLocalFile()) == normalizedPath(index.data(Qt::UserRole).toString());
    if (!currentSong) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem rowOption(option);
    initStyleOption(&rowOption, index);
    rowOption.font.setBold(true);
    painter->save();
    painter->setClipRect(option.rect);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    painter->fillRect(option.rect, QColor(255, 255, 255, hovered ? 72 : 56));
    const QRect accentRect(option.rect.left(), option.rect.top() + 6, 3,
                           qMax(0, option.rect.height() - 12));
    painter->fillRect(accentRect, QColor(113, 232, 182));
    painter->setFont(rowOption.font);
    painter->setPen(QColor(255, 255, 255));
    const QRect textRect = option.rect.adjusted(12, 0, -12, 0);
    const QString text = QFontMetrics(rowOption.font)
                             .elidedText(rowOption.text, Qt::ElideRight, qMax(0, textRect.width()));
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
    painter->restore();
}

} // namespace PlayerWidgets
