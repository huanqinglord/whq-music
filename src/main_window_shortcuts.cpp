#include "main_window.h"
#include "shortcut_manager.h"
#include "shortcut_key_edit.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QVBoxLayout>
#include <QVariant>

QWidget *MainWindow::createShortcutSettingsPage()
{
    m_shortcutManager = new ShortcutManager(this);
    auto *panel = new QWidget;
    panel->setObjectName(QStringLiteral("shortcutSettingsPanel"));
    panel->setMaximumWidth(820);
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);
    auto *description =
        new QLabel(QStringLiteral("全局快捷键 · 修改后自动保存并立即生效"), panel);
    description->setObjectName(QStringLiteral("settingsDescription"));
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto *enableAll = new QPushButton(QStringLiteral("全部启用"), panel);
    auto *disableAll = new QPushButton(QStringLiteral("全部禁用"), panel);
    auto *restore = new QPushButton(QStringLiteral("恢复默认"), panel);
    actions->addWidget(enableAll);
    actions->addWidget(disableAll);
    actions->addWidget(restore);
    actions->addStretch();
    layout->addLayout(actions);
    connect(enableAll, &QPushButton::clicked, m_shortcutManager, [this] {
        m_shortcutManager->setAllEnabled(true);
    });
    connect(disableAll, &QPushButton::clicked, m_shortcutManager, [this] {
        m_shortcutManager->setAllEnabled(false);
    });
    connect(restore, &QPushButton::clicked, m_shortcutManager, &ShortcutManager::restoreDefaults);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(12);
    grid->setColumnStretch(1, 1);
    const auto addHeader = [panel, grid](const QString &text, int column) {
        auto *label = new QLabel(text, panel);
        label->setObjectName(QStringLiteral("settingsFieldLabel"));
        grid->addWidget(label, 0, column);
    };
    addHeader(QStringLiteral("操作"), 0);
    addHeader(QStringLiteral("快捷键"), 1);
    addHeader(QStringLiteral("启用"), 2);
    addHeader(QStringLiteral("状态"), 3);
    for (int index = 0; index < m_shortcutManager->entries().size(); ++index) {
        const auto &entry = m_shortcutManager->entries().at(index);
        auto *title = new QLabel(entry.title, panel);
        title->setObjectName(QStringLiteral("settingsFieldLabel"));
        auto *editor = new ShortcutKeyEdit(entry.sequence, panel);
        editor->setAccessibleName(entry.title + QStringLiteral("快捷键"));
        editor->setMinimumWidth(160);
        auto *enabled = new QCheckBox(panel);
        enabled->setChecked(entry.enabled);
        enabled->setAccessibleName(QStringLiteral("启用") + entry.title + QStringLiteral("快捷键"));
        auto *status = new QLabel(panel);
        status->setObjectName(QStringLiteral("shortcutStatus"));
        status->setAlignment(Qt::AlignCenter);
        status->setMinimumWidth(68);
        auto *retry = new QPushButton(QStringLiteral("重试"), panel);
        retry->setObjectName(QStringLiteral("shortcutRetryButton"));
        retry->setAccessibleName(QStringLiteral("重试注册") + entry.title + QStringLiteral("快捷键"));
        retry->setCursor(Qt::PointingHandCursor);
        retry->setFixedWidth(64);
        QSizePolicy retryPolicy = retry->sizePolicy();
        retryPolicy.setRetainSizeWhenHidden(true);
        retry->setSizePolicy(retryPolicy);
        retry->hide();
        grid->addWidget(title, index + 1, 0);
        grid->addWidget(editor, index + 1, 1);
        grid->addWidget(enabled, index + 1, 2, Qt::AlignCenter);
        grid->addWidget(status, index + 1, 3);
        grid->addWidget(retry, index + 1, 4);

        const auto refresh = [this, index, editor, enabled, status, retry] {
            const auto &current = m_shortcutManager->entries().at(index);
            const QSignalBlocker editorBlocker(editor);
            const QSignalBlocker enabledBlocker(enabled);
            editor->setKeySequence(current.sequence);
            enabled->setChecked(current.enabled);
            QString state;
            switch (current.status) {
            case ShortcutManager::Status::Active:
                status->setText(QStringLiteral("已生效"));
                state = QStringLiteral("active");
                break;
            case ShortcutManager::Status::Occupied:
                status->setText(QStringLiteral("被占用"));
                state = QStringLiteral("occupied");
                break;
            case ShortcutManager::Status::Unset:
                status->setText(QStringLiteral("未设置"));
                state = QStringLiteral("unset");
                break;
            case ShortcutManager::Status::Disabled:
                status->setText(QStringLiteral("已禁用"));
                state = QStringLiteral("disabled");
                break;
            case ShortcutManager::Status::Failed:
                status->setText(QStringLiteral("注册失败"));
                state = QStringLiteral("failed");
                break;
            }
            status->setProperty("state", state);
            status->style()->unpolish(status);
            status->style()->polish(status);
            status->update();
            const bool canRetry = current.status == ShortcutManager::Status::Occupied ||
                                  current.status == ShortcutManager::Status::Failed;
            retry->setToolTip(canRetry
                                  ? QStringLiteral("<qt>%1</qt>").arg(current.failureHint.toHtmlEscaped())
                                  : QString());
            retry->setVisible(canRetry);
        };
        connect(m_shortcutManager, &ShortcutManager::changed, panel, refresh);
        connect(retry, &QPushButton::clicked, panel, [this, index] {
            m_shortcutManager->retry(index);
        });
        connect(editor, &ShortcutKeyEdit::recordingChanged, m_shortcutManager,
                &ShortcutManager::setRecording);
        connect(editor, &ShortcutKeyEdit::sequenceEdited, panel,
                [this, index](const QKeySequence &sequence) {
                    m_shortcutManager->update(index, sequence,
                                              m_shortcutManager->entries().at(index).enabled);
                });
        connect(enabled, &QCheckBox::toggled, panel, [this, index](bool checked) {
            m_shortcutManager->update(index, m_shortcutManager->entries().at(index).sequence,
                                      checked);
        });
    }
    layout->addLayout(grid);
    return panel;
}

void MainWindow::initializeShortcuts()
{
    connect(m_shortcutManager, &ShortcutManager::triggered, this, [this](const QString &action) {
        if (!isEnabled() || !centralWidget()->isEnabled() || m_exitRequested ||
            m_fileEditInProgress) {
            return;
        }
        if (action == QStringLiteral("playPause")) {
            togglePlayback();
        } else if (action == QStringLiteral("previous")) {
            playPrevious();
        } else if (action == QStringLiteral("next")) {
            playNext();
        } else if (action == QStringLiteral("volumeUp")) {
            m_volume->setValue(qMin(100, m_volume->value() + 5));
        } else if (action == QStringLiteral("volumeDown")) {
            m_volume->setValue(qMax(0, m_volume->value() - 5));
        } else if (action == QStringLiteral("mute")) {
            if (m_volume->value() > 0) {
                m_volumeBeforeMute = m_volume->value();
                m_volume->setValue(0);
            } else {
                m_volume->setValue(m_volumeBeforeMute);
            }
        } else if (action == QStringLiteral("playMode")) {
            cyclePlayMode();
        }
    });
    m_shortcutManager->initialize(setting(QStringLiteral("globalShortcuts")),
                                  [this](const QString &value) {
                                      return saveSettings({{QStringLiteral("globalShortcuts"), value}});
                                  });
}
