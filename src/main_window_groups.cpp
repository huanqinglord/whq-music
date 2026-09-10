#include "main_window.h"
#include "player_widgets.h"
#include "music_library_repository.h"
#include "playback_controller.h"
#include "app_dialog.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPropertyAnimation>
#include <QSet>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <utility>

using namespace PlayerWidgets;

void MainWindow::createGroup()
{
    if (m_groupEditor != nullptr) {
        m_groupEditor->setFocus();
        return;
    }

    const int editorRow = m_groups->count() - 1;
    auto *editorItem = new QListWidgetItem;
    editorItem->setSizeHint(QSize(0, 42));
    editorItem->setFlags(editorItem->flags() & ~Qt::ItemIsSelectable);
    m_groups->insertItem(editorRow, editorItem);
    m_groupEditor = new QLineEdit(m_groups);
    m_groupEditor->setObjectName(QStringLiteral("groupEditor"));
    m_groupEditor->setPlaceholderText(QStringLiteral("输入分组名称"));
    m_groups->setItemWidget(editorItem, m_groupEditor);
    QLineEdit *editor = m_groupEditor;
    qApp->installEventFilter(new EmptyGroupEditorDismissFilter(editor));
    connect(editor, &QLineEdit::editingFinished, this, [this, editor] {
        if (m_groupEditor != editor) {
            return;
        }
        const QString name = editor->text().trimmed();
        if (name.isEmpty()) {
            m_groupEditor = nullptr;
            loadGroups();
            return;
        }
        QString error;
        if (!m_repository->saveGroup(name, 0, error)) {
            showGroupEditorError(editor, error);
            QTimer::singleShot(0, editor, [editor] {
                editor->setFocus();
                editor->selectAll();
            });
            return;
        }
        m_groupEditor = nullptr;
        loadGroups();
    });
    m_groupEditor->setFocus();
}

void MainWindow::renameGroupInline(qint64 groupId, const QString &currentName)
{
    if (m_groupEditor != nullptr) {
        m_groupEditor->setFocus();
        return;
    }

    QListWidgetItem *groupItem = nullptr;
    for (int row = 0; row < m_groups->count(); ++row) {
        QListWidgetItem *item = m_groups->item(row);
        if (item->data(Qt::UserRole).toLongLong() == groupId) {
            groupItem = item;
            break;
        }
    }
    if (groupItem == nullptr) {
        return;
    }

    QWidget *oldRowWidget = m_groups->itemWidget(groupItem);
    m_groups->removeItemWidget(groupItem);
    if (oldRowWidget != nullptr) {
        oldRowWidget->deleteLater();
    }
    m_groupEditor = new QLineEdit(currentName, m_groups);
    m_groupEditor->setObjectName(QStringLiteral("groupEditor"));
    m_groups->setItemWidget(groupItem, m_groupEditor);
    QLineEdit *editor = m_groupEditor;
    qApp->installEventFilter(new EmptyGroupEditorDismissFilter(editor));
    connect(editor, &QLineEdit::editingFinished, this, [this, editor, groupId, currentName] {
        if (m_groupEditor != editor) {
            return;
        }
        const QString newName = editor->text().trimmed();
        if (newName.isEmpty() || newName == currentName) {
            m_groupEditor = nullptr;
            loadGroups();
            return;
        }
        QString error;
        if (!m_repository->saveGroup(newName, groupId, error)) {
            showGroupEditorError(editor, error);
            QTimer::singleShot(0, editor, [editor] {
                editor->setFocus();
                editor->selectAll();
            });
            return;
        }
        m_groupEditor = nullptr;
        loadGroups();
    });
    m_groupEditor->setFocus();
    m_groupEditor->selectAll();
}

void MainWindow::showGroupEditorError(QLineEdit *editor, const QString &message)
{
    if (editor == nullptr) {
        return;
    }
    auto *errorLabel = new QLabel(message, this);
    errorLabel->setObjectName(QStringLiteral("groupEditorError"));
    errorLabel->adjustSize();
    const QPoint editorPosition = editor->mapTo(this, QPoint(0, 0));
    const int x = editorPosition.x();
    const int y = qMax(m_titleBar->height(), editorPosition.y() - errorLabel->height() - 4);
    errorLabel->move(x, y);
    errorLabel->raise();
    errorLabel->show();

    auto *opacityEffect = new QGraphicsOpacityEffect(errorLabel);
    errorLabel->setGraphicsEffect(opacityEffect);
    auto *fadeAnimation = new QPropertyAnimation(opacityEffect, "opacity", errorLabel);
    fadeAnimation->setDuration(3000);
    fadeAnimation->setStartValue(1.0);
    fadeAnimation->setEndValue(0.0);
    connect(fadeAnimation, &QPropertyAnimation::finished, errorLabel, &QWidget::deleteLater);
    fadeAnimation->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::showGroupMenu(qint64 groupId, const QString &name, QWidget *anchor)
{
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("groupOperationMenu"));
    menu.addAction(groupId > 0 ? QStringLiteral("播放此分组") : QStringLiteral("播放全部歌曲"),
                   this, [this, groupId] {
                       playGroup(groupId);
                   });
    menu.addSeparator();
    menu.addAction(QStringLiteral("添加音乐"), this, [this, groupId] {
        addFiles(groupId);
    });
    menu.addAction(QStringLiteral("添加文件夹"), this, [this, groupId] {
        addFolder(groupId);
    });
    if (groupId > 0) {
        menu.addSeparator();
        menu.addAction(QStringLiteral("重命名"), this, [this, groupId, name] {
            renameGroupInline(groupId, name);
        });
        menu.addAction(QStringLiteral("删除分组"), this, [this, groupId] {
            QString error;
            if (!m_repository->deleteGroup(groupId, error)) {
                QMessageBox::warning(this, QStringLiteral("删除失败"), error);
                return;
            }
            loadGroups();
        });
    }
    menu.exec(anchor->mapToGlobal(QPoint(anchor->width(), 0)));
}

void MainWindow::showSongMenu(const QPoint &position)
{
    const int row = m_songsTable->indexAt(position).row();
    if (row < 0 || row >= m_songs.size()) {
        return;
    }
    // 菜单打开后音乐库可能刷新，提前固定右键目标，不使用后续选中行。
    const qint64 songId = m_songs.at(row).id;
    const QString audioPath = m_songs.at(row).path;
    // 保持菜单目标选中，但不改变当前索引，防止触发自动滚动。
    m_songsTable->selectionModel()->select(m_songsTable->model()->index(row, 0),
                                           QItemSelectionModel::ClearAndSelect |
                                               QItemSelectionModel::Rows);
    QMenu menu(this);
    menu.addAction(QStringLiteral("加入播放列表"), this, [this, songId] {
        addSongToPlaybackQueue(songId);
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("元数据修改"), this, [this, songId] {
        editSongMetadata(songId);
    });
    QAction *lyricsAction = menu.addAction(QStringLiteral("下载 / 更换歌词"));
    menu.addSeparator();
    menu.addAction(QStringLiteral("添加到分组"), this, [this, songId] {
        auto *surface = static_cast<AppSurface *>(centralWidget());
        AppDialog dialog(QStringLiteral("管理所属分组"), surface, surface->blurredRegion(surface),
                         this);
        dialog.setObjectName(QStringLiteral("groupMembershipDialog"));
        dialog.setMinimumSize(400, 360);
        dialog.resize(460, 520);
        auto *layout = dialog.bodyLayout();

        QString songName;
        for (const Song &song : std::as_const(m_songs)) {
            if (song.id == songId) {
                songName = QFileInfo(song.path).completeBaseName();
                break;
            }
        }
        auto *songLabel = new QLabel(QStringLiteral("歌曲：%1").arg(songName), &dialog);
        songLabel->setObjectName(QStringLiteral("groupMembershipSong"));
        layout->addWidget(songLabel);
        auto *searchEditor = new QLineEdit(&dialog);
        searchEditor->setPlaceholderText(QStringLiteral("搜索分组"));
        searchEditor->setClearButtonEnabled(true);
        layout->addWidget(searchEditor);

        QSet<qint64> existingGroupIds;
        QList<MusicGroup> groups;
        QString error;
        if (!m_repository->songGroups(songId, existingGroupIds, error) ||
            !m_repository->loadGroups(groups, error)) {
            QMessageBox::warning(this, QStringLiteral("读取失败"), error);
            return;
        }
        auto *groupList = new QListWidget(&dialog);
        groupList->setObjectName(QStringLiteral("groupMembershipList"));
        for (const MusicGroup &group : groups) {
            auto *item = new QListWidgetItem(group.name, groupList);
            item->setData(Qt::UserRole, group.id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(existingGroupIds.contains(group.id) ? Qt::Checked : Qt::Unchecked);
        }
        layout->addWidget(groupList, 1);
        auto *emptyLabel = new QLabel(QStringLiteral("暂无分组，请先创建分组"), &dialog);
        emptyLabel->setObjectName(QStringLiteral("groupMembershipEmpty"));
        emptyLabel->setAlignment(Qt::AlignCenter);
        emptyLabel->setVisible(groupList->count() == 0);
        layout->addWidget(emptyLabel);

        auto *selectionLabel = new QLabel(&dialog);
        selectionLabel->setObjectName(QStringLiteral("groupMembershipCount"));
        const auto updateSelectionCount = [groupList, selectionLabel] {
            int selectedCount = 0;
            for (int row = 0; row < groupList->count(); ++row) {
                if (groupList->item(row)->checkState() == Qt::Checked) {
                    ++selectedCount;
                }
            }
            selectionLabel->setText(QStringLiteral("已选择 %1 个分组").arg(selectedCount));
        };
        updateSelectionCount();
        layout->addWidget(selectionLabel);
        connect(groupList, &QListWidget::itemChanged, &dialog,
                [updateSelectionCount](QListWidgetItem *) {
                    updateSelectionCount();
                });
        connect(searchEditor, &QLineEdit::textChanged, &dialog, [groupList](const QString &text) {
            const QString keyword = text.trimmed();
            for (int row = 0; row < groupList->count(); ++row) {
                QListWidgetItem *item = groupList->item(row);
                item->setHidden(!keyword.isEmpty() &&
                                !item->text().contains(keyword, Qt::CaseInsensitive));
            }
        });

        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        buttons->button(QDialogButtonBox::Save)
            ->setObjectName(QStringLiteral("primaryDialogButton"));
        buttons->button(QDialogButtonBox::Cancel)
            ->setObjectName(QStringLiteral("secondaryDialogButton"));
        buttons->button(QDialogButtonBox::Save)->setEnabled(groupList->count() > 0);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        QList<qint64> selectedGroups;
        for (int row = 0; row < groupList->count(); ++row) {
            const QListWidgetItem *item = groupList->item(row);
            if (item->checkState() == Qt::Checked) {
                selectedGroups.append(item->data(Qt::UserRole).toLongLong());
            }
        }
        if (!m_repository->assignGroups(songId, selectedGroups, error)) {
            QMessageBox::warning(this, QStringLiteral("保存失败"), error);
            return;
        }
        loadSongs(false);
    });
    menu.addAction(QStringLiteral("从音乐库删除"), this, [this, songId] {
        QString error;
        if (!m_repository->deleteSong(songId, error)) {
            QMessageBox::warning(this, QStringLiteral("删除失败"), error);
            return;
        }
        loadSongs();
    });
    if (menu.exec(m_songsTable->viewport()->mapToGlobal(position)) == lyricsAction) {
        requestLyricsDownload(audioPath);
    }
}

void MainWindow::showQueueMenu(const QPoint &position)
{
    if (m_fileEditInProgress) {
        return;
    }
    QListWidgetItem *item = m_queue->itemAt(position);
    QMenu menu(this);
    if (item != nullptr) {
        const qint64 songId = item->data(Qt::UserRole + 1).toLongLong();
        const QString audioPath = item->data(Qt::UserRole).toString();
        menu.addAction(QStringLiteral("下载 / 更换歌词"), this, [this, audioPath] {
            requestLyricsDownload(audioPath);
        });
        menu.addAction(QStringLiteral("从播放列表移除"), this, [this, songId] {
            m_playbackController->remove(songId);
        });
        menu.addSeparator();
    }
    QAction *clear = menu.addAction(QStringLiteral("清空播放列表"), this, [this] {
        m_playbackController->clear();
    });
    clear->setEnabled(!m_playbackController->queue().isEmpty());
    menu.exec(m_queue->viewport()->mapToGlobal(position));
}
