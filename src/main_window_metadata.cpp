#include "main_window.h"
#include "player_widgets.h"
#include "music_file_utils.h"
#include "playback_controller.h"
#include "metadata_scanner.h"
#include "audio_file_editor.h"
#include "app_dialog.h"

#include <QAbstractItemView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPalette>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSizePolicy>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVariant>
#include <QVBoxLayout>
#include <QtConcurrent>

using namespace PlayerWidgets;

void MainWindow::editSongMetadata(qint64 songId)
{
    if (m_metadataReadInProgress || m_fileEditInProgress || m_libraryRefreshInProgress ||
        m_libraryImportInProgress) {
        QMessageBox::information(this, QStringLiteral("请稍候"),
                                 QStringLiteral("音乐库正在更新，请完成后再编辑。"));
        return;
    }
    QSqlQuery songQuery(m_database);
    songQuery.prepare(QStringLiteral("SELECT path FROM songs WHERE id=?"));
    songQuery.addBindValue(songId);
    if (!songQuery.exec() || !songQuery.next()) {
        QMessageBox::warning(this, QStringLiteral("读取失败"),
                             QStringLiteral("无法读取歌曲数据库记录。"));
        return;
    }
    const QString originalPath = songQuery.value(0).toString();
    songQuery.finish();
    m_metadataReadInProgress = true;
    auto *progress = new QProgressDialog(QStringLiteral("正在读取歌曲信息…"),
                                         QStringLiteral("取消"), 0, 0, this);
    progress->setWindowTitle(QStringLiteral("读取歌曲"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->show();
    auto *watcher = new QFutureWatcher<AudioMetadataSnapshot>(this);
    connect(progress, &QProgressDialog::canceled, watcher,
            &QFutureWatcher<AudioMetadataSnapshot>::cancel);
    connect(watcher, &QFutureWatcher<AudioMetadataSnapshot>::finished, this,
            [this, watcher, progress] {
                // 程序关闭进度窗口不代表用户取消，先断开该连接再关闭。
                disconnect(progress, nullptr, watcher, nullptr);
                m_metadataReadInProgress = false;
                progress->close();
                progress->deleteLater();
                if (watcher->isCanceled()) {
                    watcher->deleteLater();
                    return;
                }
                const AudioMetadataSnapshot snapshot = watcher->result();
                watcher->deleteLater();
                if (!snapshot.error.isEmpty()) {
                    QMessageBox::warning(this, QStringLiteral("读取失败"), snapshot.error);
                    return;
                }
                showMetadataEditor(snapshot);
            });
    const auto future = QtConcurrent::run(readAudioMetadata, originalPath);
    m_metadataReadFuture = future;
    watcher->setFuture(future);
}

void MainWindow::showMetadataEditor(const AudioMetadataSnapshot &snapshot)
{
    const QString originalPath = snapshot.path;
    const QFileInfo originalInfo(originalPath);
    const QString originalHash = snapshot.contentHash;
    const QJsonObject originalProperties = snapshot.properties;
    auto *surface = static_cast<AppSurface *>(centralWidget());
    AppDialog dialog(QStringLiteral("元数据修改"), surface, surface->blurredRegion(surface), this);
    dialog.setObjectName(QStringLiteral("metadataDialog"));
    dialog.setMinimumSize(600, 460);
    dialog.resize(720, 620);
    auto *layout = dialog.bodyLayout();

    auto *fileForm = new QFormLayout;
    fileForm->setHorizontalSpacing(16);
    auto *fileNameEditor = new QLineEdit(originalInfo.completeBaseName(), &dialog);
    fileNameEditor->setObjectName(QStringLiteral("metadataValueEditor"));
    fileForm->addRow(QStringLiteral("文件名"), fileNameEditor);
    layout->addLayout(fileForm);

    QStringList fileFacts;
    fileFacts.append(originalInfo.suffix().toUpper());
    fileFacts.append(QString::number(snapshot.sizeBytes / (1024.0 * 1024.0), 'f', 2) +
                     QStringLiteral(" MB"));
    if (snapshot.durationMs > 0) {
        fileFacts.append(formatTime(snapshot.durationMs));
    }
    auto *fileSummary = new QLabel(fileFacts.join(QStringLiteral("  ·  ")), &dialog);
    fileSummary->setObjectName(QStringLiteral("metadataFileSummary"));
    layout->addWidget(fileSummary);

    auto *details = new QWidget(&dialog);
    details->setObjectName(QStringLiteral("metadataDetails"));
    auto *detailsForm = new QFormLayout(details);
    detailsForm->setContentsMargins(0, 0, 0, 0);
    detailsForm->setHorizontalSpacing(16);
    detailsForm->setVerticalSpacing(6);
    const auto addDetail = [details, detailsForm](const QString &label, const QString &value) {
        auto *text = new QLabel(value, details);
        text->setTextFormat(Qt::PlainText);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        text->setWordWrap(true);
        text->setMinimumWidth(0);
        text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        detailsForm->addRow(label, text);
    };
    addDetail(QStringLiteral("路径"), originalPath);
    if (snapshot.sampleRateHz > 0) {
        addDetail(QStringLiteral("音频参数"), QStringLiteral("%1 kbps  ·  %2 Hz  ·  %3 声道")
                                                  .arg(snapshot.bitrateKbps)
                                                  .arg(snapshot.sampleRateHz)
                                                  .arg(snapshot.channels));
    }
    addDetail(QStringLiteral("SHA-256"), originalHash);
    details->hide();

    auto *metadataTable = new QTableWidget(&dialog);
    metadataTable->setObjectName(QStringLiteral("metadataTable"));
    metadataTable->setColumnCount(2);
    metadataTable->setHorizontalHeaderLabels(
        {QStringLiteral("元数据字段"), QStringLiteral("值（多个值用分号分隔）")});
    metadataTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    metadataTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    metadataTable->verticalHeader()->setVisible(false);
    metadataTable->verticalHeader()->setDefaultSectionSize(
        qMax(36, dialog.fontMetrics().height() + 16));
    metadataTable->setShowGrid(false);
    metadataTable->setAlternatingRowColors(true);
    metadataTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    const QList<QByteArray> standardKeys = {
        QByteArrayLiteral("TITLE"),      QByteArrayLiteral("ARTIST"),
        QByteArrayLiteral("ALBUM"),      QByteArrayLiteral("ALBUMARTIST"),
        QByteArrayLiteral("COMPOSER"),   QByteArrayLiteral("GENRE"),
        QByteArrayLiteral("DATE"),       QByteArrayLiteral("TRACKNUMBER"),
        QByteArrayLiteral("DISCNUMBER"), QByteArrayLiteral("COMMENT")};
    const QHash<QString, QString> standardLabels = {
        {QStringLiteral("TITLE"), QStringLiteral("标题")},
        {QStringLiteral("ARTIST"), QStringLiteral("歌手")},
        {QStringLiteral("ALBUM"), QStringLiteral("专辑")},
        {QStringLiteral("ALBUMARTIST"), QStringLiteral("专辑歌手")},
        {QStringLiteral("COMPOSER"), QStringLiteral("作曲")},
        {QStringLiteral("GENRE"), QStringLiteral("流派")},
        {QStringLiteral("DATE"), QStringLiteral("日期")},
        {QStringLiteral("TRACKNUMBER"), QStringLiteral("音轨号")},
        {QStringLiteral("DISCNUMBER"), QStringLiteral("碟片号")},
        {QStringLiteral("COMMENT"), QStringLiteral("注释")}};
    QSet<QString> displayedKeys;
    const auto addRow = [metadataTable, &displayedKeys](const QString &key, const QString &label,
                                                        const QString &value) {
        if (displayedKeys.contains(key)) {
            return;
        }
        displayedKeys.insert(key);
        const int row = metadataTable->rowCount();
        metadataTable->insertRow(row);
        auto *keyItem = new QTableWidgetItem(label);
        keyItem->setFlags(keyItem->flags() & ~Qt::ItemIsEditable);
        keyItem->setData(Qt::UserRole, key);
        metadataTable->setItem(row, 0, keyItem);
        const bool unset = value.isEmpty();
        auto *valueItem = new QTableWidgetItem(unset ? QStringLiteral("未设置") : value);
        valueItem->setData(Qt::UserRole + 1, unset);
        if (unset) {
            valueItem->setForeground(QColor(QStringLiteral("#8f94a8")));
        }
        metadataTable->setItem(row, 1, valueItem);
    };
    const auto propertyText = [&originalProperties](const QString &key) {
        QStringList values;
        for (const QJsonValue &value : originalProperties.value(key).toArray()) {
            values.append(value.toString());
        }
        return values.join(QStringLiteral("; "));
    };
    for (const QByteArray &key : standardKeys) {
        const QString keyText = QString::fromLatin1(key);
        QString value = propertyText(keyText);
        if (value.isEmpty()) {
            value = snapshot.fallbackProperties.value(keyText).toString();
        }
        addRow(keyText, standardLabels.value(keyText), value);
    }
    for (auto property = originalProperties.begin(); property != originalProperties.end();
         ++property) {
        addRow(property.key(), QStringLiteral("其他：%1").arg(property.key()),
               propertyText(property.key()));
    }
    auto *metadataSummary =
        new QLabel(QStringLiteral("双击右侧输入框进行编辑，保存后写入音乐文件。"), &dialog);
    metadataSummary->setObjectName(QStringLiteral("metadataSummary"));
    layout->addWidget(metadataSummary);
    connect(metadataTable, &QTableWidget::itemChanged, &dialog,
            [metadataTable](QTableWidgetItem *item) {
                if (item == nullptr || item->column() != 1 ||
                    !item->data(Qt::UserRole + 1).toBool() ||
                    item->text() == QStringLiteral("未设置")) {
                    return;
                }
                item->setData(Qt::UserRole + 1, false);
                item->setForeground(metadataTable->palette().color(QPalette::Text));
            });
    connect(metadataTable, &QTableWidget::cellDoubleClicked, &dialog,
            [metadataTable](int row, int column) {
                if (column != 1) {
                    return;
                }
                QTableWidgetItem *item = metadataTable->item(row, column);
                if (item != nullptr && item->data(Qt::UserRole + 1).toBool()) {
                    item->setText({});
                }
            });
    layout->addWidget(metadataTable, 1);
    auto *detailsToggle = new QPushButton(QStringLiteral("▸ 文件详情"), &dialog);
    detailsToggle->setObjectName(QStringLiteral("metadataDetailsToggle"));
    detailsToggle->setCheckable(true);
    detailsToggle->setAutoDefault(false);
    connect(detailsToggle, &QPushButton::toggled, &dialog, [details, detailsToggle](bool expanded) {
        details->setVisible(expanded);
        detailsToggle->setText(expanded ? QStringLiteral("▾ 收起文件详情")
                                        : QStringLiteral("▸ 文件详情"));
    });
    layout->addWidget(detailsToggle);
    layout->addWidget(details);
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("primaryDialogButton"));
    buttons->button(QDialogButtonBox::Cancel)
        ->setObjectName(QStringLiteral("secondaryDialogButton"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString newBaseName = fileNameEditor->text().trimmed();
    const QRegularExpression invalidName(QStringLiteral("[<>:\u0022/\\\\|?*]"));
    if (newBaseName.isEmpty() || invalidName.match(newBaseName).hasMatch() ||
        newBaseName.endsWith(QLatin1Char('.')) || newBaseName.endsWith(QLatin1Char(' '))) {
        QMessageBox::warning(this, QStringLiteral("文件名无效"),
                             QStringLiteral("文件名包含非法字符或以点、空格结尾。"));
        return;
    }

    QJsonObject metadataJson = originalProperties;
    for (int row = 0; row < metadataTable->rowCount(); ++row) {
        const QString key = metadataTable->item(row, 0)->data(Qt::UserRole).toString();
        const QTableWidgetItem *valueItem = metadataTable->item(row, 1);
        const QString value =
            valueItem->data(Qt::UserRole + 1).toBool() ? QString() : valueItem->text().trimmed();
        if (value == propertyText(key).trimmed()) {
            continue;
        }
        if (value.isEmpty()) {
            metadataJson.remove(key);
            continue;
        }
        QJsonArray jsonValues;
        for (const QString &part : value.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const QString trimmedPart = part.trimmed();
            if (!trimmedPart.isEmpty()) {
                jsonValues.append(trimmedPart);
            }
        }
        metadataJson.insert(key, jsonValues);
    }

    const QString newPath =
        originalInfo.dir().filePath(newBaseName + QLatin1Char('.') + originalInfo.suffix());
    AudioEditRequest request;
    request.originalPath = originalPath;
    request.targetPath = newPath;
    request.databasePath = m_database.databaseName();
    request.expectedHash = originalHash;
    request.properties = metadataJson;
    auto *progress = new QProgressDialog(QStringLiteral("正在保存歌曲信息…"), {}, 0, 0, this);
    progress->setWindowTitle(QStringLiteral("保存歌曲"));
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setCancelButton(nullptr);
    progress->setAutoClose(false);
    progress->show();
    startAudioFileEdit(request, [this, progress](const AudioEditResult &result, bool) {
        progress->close();
        progress->deleteLater();
        if (!result.success) {
            QMessageBox::warning(this, QStringLiteral("保存失败"), result.message);
        } else if (!result.warning.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("保存完成"), result.warning);
        }
    });
}

void MainWindow::startAudioFileEdit(
    const AudioEditRequest &request,
    const std::function<void(const AudioEditResult &, bool)> &completion)
{
    if (m_metadataReadInProgress || m_fileEditInProgress || m_libraryImportInProgress ||
        m_libraryRefreshInProgress) {
        completion({false, request.originalPath, QStringLiteral("音乐库正在更新，请稍后重试。")},
                   false);
        return;
    }
    m_fileEditInProgress = true;
    m_metadataScanner->suspend();
    m_refreshLibraryButton->setEnabled(false);
    updateTaskbarToolbar();
    auto *preparation = new QFutureWatcher<PreparedAudioEdit>(this);
    connect(preparation, &QFutureWatcher<PreparedAudioEdit>::finished, this,
            [this, preparation, completion] {
                const PreparedAudioEdit prepared = preparation->result();
                preparation->deleteLater();
                if (!prepared.error.isEmpty() || prepared.needsOverwrite) {
                    m_fileEditInProgress = false;
                    m_metadataScanner->resume();
                    m_refreshLibraryButton->setEnabled(true);
                    updateTaskbarToolbar();
                    completion({false, prepared.request.originalPath, prepared.error},
                               prepared.needsOverwrite);
                    return;
                }
                // 副本准备期间继续播放；仅在替换文件前释放音源并记住此刻的状态。
                const bool current = m_player->source().isLocalFile() &&
                                     normalizedPath(m_player->source().toLocalFile()) ==
                                         normalizedPath(prepared.request.originalPath);
                const qint64 position = m_player->position();
                const auto state = m_player->playbackState();
                if (current) {
                    m_playbackController->prepareFileEdit();
                }
                auto *commit = new QFutureWatcher<AudioEditResult>(this);
                connect(commit, &QFutureWatcher<AudioEditResult>::finished, this,
                        [this, commit, completion, current, position, state] {
                            const AudioEditResult result = commit->result();
                            commit->deleteLater();
                            m_fileEditInProgress = false;
                            loadSongs();
                            if (current && !result.actualPath.isEmpty()) {
                                m_detailTitle->setText(
                                    QFileInfo(result.actualPath).completeBaseName());
                                loadLyrics(result.actualPath);
                                m_playbackController->restoreFileEdit(result.actualPath, position,
                                                                      state);
                            } else if (current) {
                                m_playbackController->clear();
                            }
                            m_metadataScanner->resume();
                            scanSongsWithoutArtist();
                            m_refreshLibraryButton->setEnabled(true);
                            updateTaskbarToolbar();
                            completion(result, false);
                        });
                const auto future = QtConcurrent::run(commitAudioEdit, prepared);
                m_editFuture = future;
                commit->setFuture(future);
            });
    const auto future = QtConcurrent::run(prepareAudioEdit, request);
    m_editFuture = future;
    preparation->setFuture(future);
}
